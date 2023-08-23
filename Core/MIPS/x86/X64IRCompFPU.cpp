// Copyright (c) 2023- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "ppsspp_config.h"
#if PPSSPP_ARCH(X86) || PPSSPP_ARCH(AMD64)

#include "Core/MIPS/x86/X64IRJit.h"
#include "Core/MIPS/x86/X64IRRegCache.h"

// This file contains compilation for floating point related instructions.
//
// All functions should have CONDITIONAL_DISABLE, so we can narrow things down to a file quickly.
// Currently known non working ones should have DISABLE.  No flags because that's in IR already.

// #define CONDITIONAL_DISABLE { CompIR_Generic(inst); return; }
#define CONDITIONAL_DISABLE {}
#define DISABLE { CompIR_Generic(inst); return; }
#define INVALIDOP { _assert_msg_(false, "Invalid IR inst %d", (int)inst.op); CompIR_Generic(inst); return; }

namespace MIPSComp {

using namespace Gen;
using namespace X64IRJitConstants;

alignas(16) const u32 reverseQNAN[4] = { 0x803FFFFF, 0x803FFFFF, 0x803FFFFF, 0x803FFFFF };

void X64JitBackend::CompIR_FArith(IRInst inst) {
	CONDITIONAL_DISABLE;

	switch (inst.op) {
	case IROp::FAdd:
		regs_.Map(inst);
		if (inst.dest == inst.src1) {
			ADDSS(regs_.FX(inst.dest), regs_.F(inst.src2));
		} else if (inst.dest == inst.src2) {
			ADDSS(regs_.FX(inst.dest), regs_.F(inst.src1));
		} else if (cpu_info.bAVX) {
			VADDSS(regs_.FX(inst.dest), regs_.FX(inst.src1), regs_.F(inst.src2));
		} else {
			MOVAPS(regs_.FX(inst.dest), regs_.F(inst.src1));
			ADDSS(regs_.FX(inst.dest), regs_.F(inst.src2));
		}
		break;

	case IROp::FSub:
		if (inst.dest == inst.src1) {
			regs_.Map(inst);
			SUBSS(regs_.FX(inst.dest), regs_.F(inst.src2));
		} else if (cpu_info.bAVX) {
			regs_.Map(inst);
			VSUBSS(regs_.FX(inst.dest), regs_.FX(inst.src1), regs_.F(inst.src2));
		} else if (inst.dest == inst.src2) {
			X64Reg tempReg = regs_.MapWithFPRTemp(inst);
			MOVAPS(tempReg, regs_.F(inst.src2));
			MOVAPS(regs_.FX(inst.dest), regs_.F(inst.src1));
			SUBSS(regs_.FX(inst.dest), R(tempReg));
		} else {
			regs_.Map(inst);
			MOVAPS(regs_.FX(inst.dest), regs_.F(inst.src1));
			SUBSS(regs_.FX(inst.dest), regs_.F(inst.src2));
		}
		break;

	case IROp::FMul:
	{
		X64Reg tempReg = regs_.MapWithFPRTemp(inst);

		// tempReg = !my_isnan(src1) && !my_isnan(src2)
		MOVSS(tempReg, regs_.F(inst.src1));
		CMPORDSS(tempReg, regs_.F(inst.src2));
		if (inst.dest == inst.src1) {
			MULSS(regs_.FX(inst.dest), regs_.F(inst.src2));
		} else if (inst.dest == inst.src2) {
			MULSS(regs_.FX(inst.dest), regs_.F(inst.src1));
		} else if (cpu_info.bAVX) {
			VMULSS(regs_.FX(inst.dest), regs_.FX(inst.src1), regs_.F(inst.src2));
		} else {
			MOVAPS(regs_.FX(inst.dest), regs_.F(inst.src1));
			MULSS(regs_.FX(inst.dest), regs_.F(inst.src2));
		}

		// Abuse a lane of tempReg to remember dest: NAN, NAN, res, res.
		SHUFPS(tempReg, regs_.F(inst.dest), 0);
		// dest = my_isnan(dest) && !my_isnan(src1) && !my_isnan(src2)
		CMPUNORDSS(regs_.FX(inst.dest), regs_.F(inst.dest));
		ANDPS(regs_.FX(inst.dest), R(tempReg));
		// At this point fd = FFFFFFFF if non-NAN inputs produced a NAN output.
		// We'll AND it with the inverse QNAN bits to clear (00000000 means no change.)
		if (RipAccessible(&reverseQNAN)) {
			ANDPS(regs_.FX(inst.dest), M(&reverseQNAN));  // rip accessible
		} else {
			MOV(PTRBITS, R(SCRATCH1), ImmPtr(&reverseQNAN));
			ANDPS(regs_.FX(inst.dest), MatR(SCRATCH1));
		}
		// ANDN is backwards, which is why we saved XMM0 to start.  Now put it back.
		SHUFPS(tempReg, R(tempReg), 0xFF);
		ANDNPS(regs_.FX(inst.dest), R(tempReg));
		break;
	}

	case IROp::FDiv:
	case IROp::FSqrt:
	case IROp::FNeg:
		CompIR_Generic(inst);
		break;

	default:
		INVALIDOP;
		break;
	}
}

void X64JitBackend::CompIR_FAssign(IRInst inst) {
	CONDITIONAL_DISABLE;

	switch (inst.op) {
	case IROp::FMov:
		if (inst.dest != inst.src1) {
			regs_.Map(inst);
			MOVAPS(regs_.FX(inst.dest), regs_.F(inst.src1));
		}
		break;

	case IROp::FAbs:
	case IROp::FSign:
		CompIR_Generic(inst);
		break;

	default:
		INVALIDOP;
		break;
	}
}

void X64JitBackend::CompIR_FCompare(IRInst inst) {
	CONDITIONAL_DISABLE;

	constexpr IRReg IRREG_VFPU_CC = IRREG_VFPU_CTRL_BASE + VFPU_CTRL_CC;

	switch (inst.op) {
	case IROp::FCmp:
		switch (inst.dest) {
		case IRFpCompareMode::False:
			regs_.SetGPRImm(IRREG_FPCOND, 0);
			break;

		case IRFpCompareMode::EitherUnordered:
			regs_.MapWithExtra(inst, { { 'G', IRREG_FPCOND, 1, MIPSMap::NOINIT } });
			UCOMISS(regs_.FX(inst.src1), regs_.F(inst.src2));
			// PF = UNORDERED.
			SETcc(CC_P, R(SCRATCH1));
			MOVZX(32, 8, regs_.RX(IRREG_FPCOND), R(SCRATCH1));
			break;

		case IRFpCompareMode::EqualOrdered:
			regs_.MapWithExtra(inst, { { 'G', IRREG_FPCOND, 1, MIPSMap::NOINIT } });
			// Clear the upper bits of SCRATCH1 so we can AND later.
			XOR(32, R(SCRATCH1), R(SCRATCH1));
			UCOMISS(regs_.FX(inst.src1), regs_.F(inst.src2));
			// E/ZF = EQUAL or UNORDERED (not exactly what we want.)
			SETcc(CC_E, R(SCRATCH1));
			if (regs_.HasLowSubregister(regs_.RX(IRREG_FPCOND))) {
				// NP/!PF = ORDERED.
				SETcc(CC_NP, regs_.R(IRREG_FPCOND));
				AND(32, regs_.R(IRREG_FPCOND), R(SCRATCH1));
			} else {
				MOVZX(32, 8, regs_.RX(IRREG_FPCOND), R(SCRATCH1));
				// Neither of those affected flags, luckily.
				// NP/!PF = ORDERED.
				SETcc(CC_NP, R(SCRATCH1));
				AND(32, regs_.R(IRREG_FPCOND), R(SCRATCH1));
			}
			break;

		case IRFpCompareMode::EqualUnordered:
			regs_.MapWithExtra(inst, { { 'G', IRREG_FPCOND, 1, MIPSMap::NOINIT } });
			UCOMISS(regs_.FX(inst.src1), regs_.F(inst.src2));
			// E/ZF = EQUAL or UNORDERED.
			SETcc(CC_E, R(SCRATCH1));
			MOVZX(32, 8, regs_.RX(IRREG_FPCOND), R(SCRATCH1));
			break;

		case IRFpCompareMode::LessEqualOrdered:
			regs_.MapWithExtra(inst, { { 'G', IRREG_FPCOND, 1, MIPSMap::NOINIT } });
			UCOMISS(regs_.FX(inst.src2), regs_.F(inst.src1));
			// AE/!CF = GREATER or EQUAL (src2/src1 reversed.)
			SETcc(CC_AE, R(SCRATCH1));
			MOVZX(32, 8, regs_.RX(IRREG_FPCOND), R(SCRATCH1));
			break;

		case IRFpCompareMode::LessEqualUnordered:
			regs_.MapWithExtra(inst, { { 'G', IRREG_FPCOND, 1, MIPSMap::NOINIT } });
			UCOMISS(regs_.FX(inst.src1), regs_.F(inst.src2));
			// BE/CF||ZF = LESS THAN or EQUAL or UNORDERED.
			SETcc(CC_BE, R(SCRATCH1));
			MOVZX(32, 8, regs_.RX(IRREG_FPCOND), R(SCRATCH1));
			break;

		case IRFpCompareMode::LessOrdered:
			regs_.MapWithExtra(inst, { { 'G', IRREG_FPCOND, 1, MIPSMap::NOINIT } });
			UCOMISS(regs_.FX(inst.src2), regs_.F(inst.src1));
			// A/!CF&&!ZF = GREATER (src2/src1 reversed.)
			SETcc(CC_A, R(SCRATCH1));
			MOVZX(32, 8, regs_.RX(IRREG_FPCOND), R(SCRATCH1));
			break;

		case IRFpCompareMode::LessUnordered:
			regs_.MapWithExtra(inst, { { 'G', IRREG_FPCOND, 1, MIPSMap::NOINIT } });
			UCOMISS(regs_.FX(inst.src1), regs_.F(inst.src2));
			// B/CF = LESS THAN or UNORDERED.
			SETcc(CC_B, R(SCRATCH1));
			MOVZX(32, 8, regs_.RX(IRREG_FPCOND), R(SCRATCH1));
			break;
		}
		break;

	case IROp::FCmovVfpuCC:
	case IROp::FCmpVfpuBit:
	case IROp::FCmpVfpuAggregate:
		CompIR_Generic(inst);
		break;

	default:
		INVALIDOP;
		break;
	}
}

void X64JitBackend::CompIR_FCondAssign(IRInst inst) {
	CONDITIONAL_DISABLE;

	FixupBranch skipNAN;
	FixupBranch finishNAN;
	FixupBranch negativeSigns;
	FixupBranch finishNANSigns;
	X64Reg tempReg = INVALID_REG;
	switch (inst.op) {
	case IROp::FMin:
		tempReg = regs_.GetAndLockTempR();
		regs_.Map(inst);
		UCOMISS(regs_.FX(inst.src1), regs_.F(inst.src1));
		skipNAN = J_CC(CC_NP, true);

		// Slow path: NAN case.  Check if both are negative.
		MOVD_xmm(R(tempReg), regs_.FX(inst.src1));
		MOVD_xmm(R(SCRATCH1), regs_.FX(inst.src2));
		TEST(32, R(SCRATCH1), R(tempReg));
		negativeSigns = J_CC(CC_S);

		// Okay, one or the other positive.
		CMP(32, R(tempReg), R(SCRATCH1));
		CMOVcc(32, tempReg, R(SCRATCH1), CC_G);
		MOVD_xmm(regs_.FX(inst.dest), R(tempReg));
		finishNAN = J();

		// Okay, both negative.
		SetJumpTarget(negativeSigns);
		CMP(32, R(tempReg), R(SCRATCH1));
		CMOVcc(32, tempReg, R(SCRATCH1), CC_L);
		MOVD_xmm(regs_.FX(inst.dest), R(tempReg));
		finishNANSigns = J();

		SetJumpTarget(skipNAN);
		if (cpu_info.bAVX) {
			VMINSS(regs_.FX(inst.dest), regs_.FX(inst.src1), regs_.F(inst.src2));
		} else {
			MOVAPS(regs_.FX(inst.dest), regs_.F(inst.src1));
			MINSS(regs_.FX(inst.dest), regs_.F(inst.src2));
		}
		SetJumpTarget(finishNAN);
		SetJumpTarget(finishNANSigns);
		break;

	case IROp::FMax:
		tempReg = regs_.GetAndLockTempR();
		regs_.Map(inst);
		UCOMISS(regs_.FX(inst.src1), regs_.F(inst.src1));
		skipNAN = J_CC(CC_NP, true);

		// Slow path: NAN case.  Check if both are negative.
		MOVD_xmm(R(tempReg), regs_.FX(inst.src1));
		MOVD_xmm(R(SCRATCH1), regs_.FX(inst.src2));
		TEST(32, R(SCRATCH1), R(tempReg));
		negativeSigns = J_CC(CC_S);

		// Okay, one or the other positive.
		CMP(32, R(tempReg), R(SCRATCH1));
		CMOVcc(32, tempReg, R(SCRATCH1), CC_L);
		MOVD_xmm(regs_.FX(inst.dest), R(tempReg));
		finishNAN = J();

		// Okay, both negative.
		SetJumpTarget(negativeSigns);
		CMP(32, R(tempReg), R(SCRATCH1));
		CMOVcc(32, tempReg, R(SCRATCH1), CC_G);
		MOVD_xmm(regs_.FX(inst.dest), R(tempReg));
		finishNANSigns = J();

		SetJumpTarget(skipNAN);
		if (cpu_info.bAVX) {
			VMAXSS(regs_.FX(inst.dest), regs_.FX(inst.src1), regs_.F(inst.src2));
		} else {
			MOVAPS(regs_.FX(inst.dest), regs_.F(inst.src1));
			MAXSS(regs_.FX(inst.dest), regs_.F(inst.src2));
		}
		SetJumpTarget(finishNAN);
		SetJumpTarget(finishNANSigns);
		break;

	default:
		INVALIDOP;
		break;
	}
}

void X64JitBackend::CompIR_FCvt(IRInst inst) {
	CONDITIONAL_DISABLE;

	switch (inst.op) {
	case IROp::FCvtWS:
	case IROp::FCvtSW:
	case IROp::FCvtScaledWS:
	case IROp::FCvtScaledSW:
		CompIR_Generic(inst);
		break;

	default:
		INVALIDOP;
		break;
	}
}

void X64JitBackend::CompIR_FRound(IRInst inst) {
	CONDITIONAL_DISABLE;

	switch (inst.op) {
	case IROp::FRound:
	case IROp::FTrunc:
	case IROp::FCeil:
	case IROp::FFloor:
		CompIR_Generic(inst);
		break;

	default:
		INVALIDOP;
		break;
	}
}

void X64JitBackend::CompIR_FSat(IRInst inst) {
	CONDITIONAL_DISABLE;

	switch (inst.op) {
	case IROp::FSat0_1:
	case IROp::FSatMinus1_1:
		CompIR_Generic(inst);
		break;

	default:
		INVALIDOP;
		break;
	}
}

void X64JitBackend::CompIR_FSpecial(IRInst inst) {
	CONDITIONAL_DISABLE;

	switch (inst.op) {
	case IROp::FSin:
	case IROp::FCos:
	case IROp::FRSqrt:
	case IROp::FRecip:
	case IROp::FAsin:
		CompIR_Generic(inst);
		break;

	default:
		INVALIDOP;
		break;
	}
}

void X64JitBackend::CompIR_RoundingMode(IRInst inst) {
	CONDITIONAL_DISABLE;

	switch (inst.op) {
	case IROp::RestoreRoundingMode:
		RestoreRoundingMode();
		break;

	case IROp::ApplyRoundingMode:
		ApplyRoundingMode();
		break;

	case IROp::UpdateRoundingMode:
		// TODO: We might want to do something here?
		break;

	default:
		INVALIDOP;
		break;
	}
}

} // namespace MIPSComp

#endif
