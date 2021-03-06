// Copyright 2014 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include "Common/Arm64Emitter.h"
#include "Common/Common.h"

#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/PPCTables.h"
#include "Core/PowerPC/JitArm64/Jit.h"
#include "Core/PowerPC/JitArm64/JitAsm.h"

FixupBranch JitArm64::JumpIfCRFieldBit(int field, int bit, bool jump_if_set)
{
	ARM64Reg WA = gpr.GetReg();
	ARM64Reg XA = EncodeRegTo64(WA);

	FixupBranch branch;
	switch (bit)
	{
	case CR_SO_BIT:  // check bit 61 set
		LDR(INDEX_UNSIGNED, XA, X29, PPCSTATE_OFF(cr_val[field]));
		branch = jump_if_set ? TBNZ(XA, 61) : TBZ(XA, 61);
	break;
	case CR_EQ_BIT:  // check bits 31-0 == 0
		LDR(INDEX_UNSIGNED, WA, X29, PPCSTATE_OFF(cr_val[field]));
		branch = jump_if_set ? CBZ(WA) : CBNZ(WA);
	break;
	case CR_GT_BIT:  // check val > 0
		LDR(INDEX_UNSIGNED, XA, X29, PPCSTATE_OFF(cr_val[field]));
		CMP(XA, SP);
		branch = B(jump_if_set ? CC_GT : CC_LE);
	break;
	case CR_LT_BIT:  // check bit 62 set
		LDR(INDEX_UNSIGNED, XA, X29, PPCSTATE_OFF(cr_val[field]));
		branch = jump_if_set ? TBNZ(XA, 62) : TBZ(XA, 62);
	break;
	default:
		_assert_msg_(DYNA_REC, false, "Invalid CR bit");
	}

	gpr.Unlock(WA);
	return branch;
}

void JitArm64::mtmsr(UGeckoInstruction inst)
{
	INSTRUCTION_START
	// Don't interpret this, if we do we get thrown out
	//JITDISABLE(bJITSystemRegistersOff)

	STR(INDEX_UNSIGNED, gpr.R(inst.RS), X29, PPCSTATE_OFF(msr));

	gpr.Flush(FlushMode::FLUSH_ALL);
	fpr.Flush(FlushMode::FLUSH_ALL);

	WriteExit(js.compilerPC + 4);
}

void JitArm64::mfmsr(UGeckoInstruction inst)
{
	INSTRUCTION_START
	JITDISABLE(bJITSystemRegistersOff);

	LDR(INDEX_UNSIGNED, gpr.R(inst.RD), X29, PPCSTATE_OFF(msr));
}

void JitArm64::mcrf(UGeckoInstruction inst)
{
	INSTRUCTION_START
	JITDISABLE(bJITSystemRegistersOff);

	if (inst.CRFS != inst.CRFD)
	{
		ARM64Reg WA = gpr.GetReg();
		ARM64Reg XA = EncodeRegTo64(WA);
		LDR(INDEX_UNSIGNED, XA, X29, PPCSTATE_OFF(cr_val[inst.CRFS]));
		STR(INDEX_UNSIGNED, XA, X29, PPCSTATE_OFF(cr_val[inst.CRFD]));
		gpr.Unlock(WA);
	}
}

void JitArm64::mfsr(UGeckoInstruction inst)
{
	INSTRUCTION_START
	JITDISABLE(bJITSystemRegistersOff);

	LDR(INDEX_UNSIGNED, gpr.R(inst.RD), X29, PPCSTATE_OFF(sr[inst.SR]));
}

void JitArm64::mtsr(UGeckoInstruction inst)
{
	INSTRUCTION_START
	JITDISABLE(bJITSystemRegistersOff);

	STR(INDEX_UNSIGNED, gpr.R(inst.RS), X29, PPCSTATE_OFF(sr[inst.SR]));
}

void JitArm64::mfsrin(UGeckoInstruction inst)
{
	INSTRUCTION_START
	JITDISABLE(bJITSystemRegistersOff);

	ARM64Reg index = gpr.GetReg();
	ARM64Reg index64 = EncodeRegTo64(index);
	ARM64Reg RB = gpr.R(inst.RB);

	UBFM(index, RB, 28, 31);
	ADD(index64, X29, index64, ArithOption(index64, ST_LSL, 2));
	LDR(INDEX_UNSIGNED, gpr.R(inst.RD), index64, PPCSTATE_OFF(sr[0]));

	gpr.Unlock(index);
}

void JitArm64::mtsrin(UGeckoInstruction inst)
{
	INSTRUCTION_START
	JITDISABLE(bJITSystemRegistersOff);

	ARM64Reg index = gpr.GetReg();
	ARM64Reg index64 = EncodeRegTo64(index);
	ARM64Reg RB = gpr.R(inst.RB);

	UBFM(index, RB, 28, 31);
	ADD(index64, X29, index64, ArithOption(index64, ST_LSL, 2));
	STR(INDEX_UNSIGNED, gpr.R(inst.RD), index64, PPCSTATE_OFF(sr[0]));

	gpr.Unlock(index);
}

void JitArm64::twx(UGeckoInstruction inst)
{
	INSTRUCTION_START
	JITDISABLE(bJITIntegerOff);

	s32 a = inst.RA;

	ARM64Reg WA = gpr.GetReg();

	if (inst.OPCD == 3) // twi
	{
		if (inst.SIMM_16 >= 0 && inst.SIMM_16 < 4096)
		{
			// Can fit in immediate in to the instruction encoding
			CMP(gpr.R(a), inst.SIMM_16);
		}
		else
		{
			MOVI2R(WA, (s32)(s16)inst.SIMM_16);
			CMP(gpr.R(a), WA);
		}
	}
	else // tw
	{
		CMP(gpr.R(a), gpr.R(inst.RB));
	}

	std::vector<FixupBranch> fixups;
	CCFlags conditions[] = { CC_LT, CC_GT, CC_EQ, CC_VC, CC_VS };

	for (int i = 0; i < 5; i++)
	{
		if (inst.TO & (1 << i))
		{
			FixupBranch f = B(conditions[i]);
			fixups.push_back(f);
		}
	}
	FixupBranch dont_trap = B();

	for (const FixupBranch& fixup : fixups)
	{
		SetJumpTarget(fixup);
	}

	gpr.Flush(FlushMode::FLUSH_MAINTAIN_STATE);
	fpr.Flush(FlushMode::FLUSH_MAINTAIN_STATE);

	LDR(INDEX_UNSIGNED, WA, X29, PPCSTATE_OFF(Exceptions));
	ORR(WA, WA, 24, 0); // Same as WA | EXCEPTION_PROGRAM
	STR(INDEX_UNSIGNED, WA, X29, PPCSTATE_OFF(Exceptions));

	MOVI2R(WA, js.compilerPC);

	// WA is unlocked in this function
	WriteExceptionExit(WA);

	SetJumpTarget(dont_trap);

	if (!analyzer.HasOption(PPCAnalyst::PPCAnalyzer::OPTION_CONDITIONAL_CONTINUE))
	{
		gpr.Flush(FlushMode::FLUSH_ALL);
		fpr.Flush(FlushMode::FLUSH_ALL);
		WriteExit(js.compilerPC + 4);
	}
}

void JitArm64::mfspr(UGeckoInstruction inst)
{
	INSTRUCTION_START
	JITDISABLE(bJITSystemRegistersOff);

	u32 iIndex = (inst.SPRU << 5) | (inst.SPRL & 0x1F);
	switch (iIndex)
	{
	case SPR_XER:
	{
		gpr.BindToRegister(inst.RD, false);
		ARM64Reg RD = gpr.R(inst.RD);
		ARM64Reg WA = gpr.GetReg();
		LDRH(INDEX_UNSIGNED, RD, X29, PPCSTATE_OFF(xer_stringctrl));
		LDRB(INDEX_UNSIGNED, WA, X29, PPCSTATE_OFF(xer_ca));
		ORR(RD, RD, WA, ArithOption(WA, ST_LSL, XER_CA_SHIFT));
		LDRB(INDEX_UNSIGNED, WA, X29, PPCSTATE_OFF(xer_so_ov));
		ORR(RD, RD, WA, ArithOption(WA, ST_LSL, XER_OV_SHIFT));
		gpr.Unlock(WA);
	}
	break;
	case SPR_WPAR:
	case SPR_DEC:
	case SPR_TL:
	case SPR_TU:
		FALLBACK_IF(true);
	default:
		gpr.BindToRegister(inst.RD, false);
		ARM64Reg RD = gpr.R(inst.RD);
		LDR(INDEX_UNSIGNED, RD, X29, PPCSTATE_OFF(spr) + iIndex * 4);
		break;
	}
}

void JitArm64::mftb(UGeckoInstruction inst)
{
	INSTRUCTION_START
	JITDISABLE(bJITSystemRegistersOff);
	mfspr(inst);
}

void JitArm64::mtspr(UGeckoInstruction inst)
{
	INSTRUCTION_START
	JITDISABLE(bJITSystemRegistersOff);

	u32 iIndex = (inst.SPRU << 5) | (inst.SPRL & 0x1F);

	switch (iIndex)
	{
	case SPR_DMAU:

	case SPR_SPRG0:
	case SPR_SPRG1:
	case SPR_SPRG2:
	case SPR_SPRG3:

	case SPR_SRR0:
	case SPR_SRR1:
		// These are safe to do the easy way, see the bottom of this function.
	break;

	case SPR_LR:
	case SPR_CTR:
	case SPR_GQR0:
	case SPR_GQR0 + 1:
	case SPR_GQR0 + 2:
	case SPR_GQR0 + 3:
	case SPR_GQR0 + 4:
	case SPR_GQR0 + 5:
	case SPR_GQR0 + 6:
	case SPR_GQR0 + 7:
		// These are safe to do the easy way, see the bottom of this function.
	break;
	case SPR_XER:
	{
		ARM64Reg RD = gpr.R(inst.RD);
		ARM64Reg WA = gpr.GetReg();
		AND(WA, RD, 24, 30);
		STRH(INDEX_UNSIGNED, WA, X29, PPCSTATE_OFF(xer_stringctrl));
		UBFM(WA, RD, XER_CA_SHIFT, XER_CA_SHIFT + 1);
		STRB(INDEX_UNSIGNED, WA, X29, PPCSTATE_OFF(xer_ca));
		UBFM(WA, RD, XER_OV_SHIFT, 31); // Same as WA = RD >> XER_OV_SHIFT
		STRB(INDEX_UNSIGNED, WA, X29, PPCSTATE_OFF(xer_so_ov));
		gpr.Unlock(WA);
	}
	break;
	default:
		FALLBACK_IF(true);
	}

	// OK, this is easy.
	ARM64Reg RD = gpr.R(inst.RD);
	STR(INDEX_UNSIGNED, RD, X29,  PPCSTATE_OFF(spr) + iIndex * 4);
}
