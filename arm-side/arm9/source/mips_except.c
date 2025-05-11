/*
 * This file is part of the DS communication library for the Supercard DSTwo.
 *
 * Copyright 2017 Nebuleon Fumika <nebuleon.fumika@gmail.com>
 *
 * It is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * It is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with it.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <nds.h>
#include <stdint.h>
#include <stddef.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "card_protocol.h"
#include "video.h"

/*
 * mips_except.c: Displays information about an exception that was raised on
 * the Supercard, including a disassembly of instructions at the point where
 * the exception was raised, and the values of all registers.
 *
 * The canonical form of all instructions is shown, and the short form of an
 * instruction is shown if a compiler would emit a canonical instruction for
 * it:
 *
 * - NOP for SLL $0, $0, 0;
 * - LI Rt, Imm16 for ORI Rt, $0, Imm16, ADDIU Rt, $0, Imm16 and OR Rd, $0, $0;
 * - MOVE Rd, Rs for OR Rd, Rs, $0 and ADDU Rd, Rs, $0;
 * - B Target for BEQ $0, $0, Target, BLEZ $0, Target and BGEZ $0, Target;
 * - BAL Target for BGEZAL $0, Target.
 *
 * Other instructions may be used for those purposes, but are much rarer. To
 * display short forms for other instructions, define UNLIKELY_SHORT_FORMS.
 */

#define REG_COUNT 29

static const uint8_t reg_nums[REG_COUNT] = {
	 1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16,
	17, 18, 19, 20, 21, 22, 23, 24, 25, 28, 29, 30, 31
};

static const size_t reg_offs[REG_COUNT] = {
	offsetof(struct card_reply_mips_exception, at),
	offsetof(struct card_reply_mips_exception, v0),
	offsetof(struct card_reply_mips_exception, v1),
	offsetof(struct card_reply_mips_exception, a0),
	offsetof(struct card_reply_mips_exception, a1),
	offsetof(struct card_reply_mips_exception, a2),
	offsetof(struct card_reply_mips_exception, a3),
	offsetof(struct card_reply_mips_exception, t0),
	offsetof(struct card_reply_mips_exception, t1),
	offsetof(struct card_reply_mips_exception, t2),
	offsetof(struct card_reply_mips_exception, t3),
	offsetof(struct card_reply_mips_exception, t4),
	offsetof(struct card_reply_mips_exception, t5),
	offsetof(struct card_reply_mips_exception, t6),
	offsetof(struct card_reply_mips_exception, t7),
	offsetof(struct card_reply_mips_exception, s0),
	offsetof(struct card_reply_mips_exception, s1),
	offsetof(struct card_reply_mips_exception, s2),
	offsetof(struct card_reply_mips_exception, s3),
	offsetof(struct card_reply_mips_exception, s4),
	offsetof(struct card_reply_mips_exception, s5),
	offsetof(struct card_reply_mips_exception, s6),
	offsetof(struct card_reply_mips_exception, s7),
	offsetof(struct card_reply_mips_exception, t8),
	offsetof(struct card_reply_mips_exception, t9),
	offsetof(struct card_reply_mips_exception, gp),
	offsetof(struct card_reply_mips_exception, sp),
	offsetof(struct card_reply_mips_exception, fp),
	offsetof(struct card_reply_mips_exception, ra)
};

static const char* excode_desc(uint32_t excode)
{
	switch (excode) {
	case  1: return "Store disallowed by TLB";
	case  2: return "TLB load exception";
	case  3: return "TLB store exception";
	case  4: return "Unaligned load";
	case  5: return "Unaligned store";
	case  6: return "Instruction fetch bus error";
	case  7: return "Data reference bus error";
	case  9: return "Breakpoint";
	case 10: return "Unknown instruction";
	case 11: return "Coprocessor unusable";
	case 12: return "Arithmetic overflow";
	case 13: return "Trap";
	case 15: return "Floating point exception";
	case 18: return "Coprocessor 2 exception";
	case 23: return "Watched data reference";
	case 24: return "Machine check exception";
	case 30: return "Cache consistency error";
	default: return "Unknown exception";
	}
}

static bool disassemble_mips32_special(uint32_t op, uint32_t loc);
static bool disassemble_mips32_regimm(uint32_t op, uint32_t loc);
static bool disassemble_mips32_cop0(uint32_t op, uint32_t loc);
static bool disassemble_mips32_pref(uint32_t op, uint32_t loc);
static bool disassemble_mips32_cache(uint32_t op, uint32_t loc);
static bool disassemble_mips32_i(uint32_t op, uint32_t loc);
static bool disassemble_mips32_special2(uint32_t op, uint32_t loc);
static bool disassemble_mips32_j(uint32_t op, uint32_t loc);

static uint32_t rel_jump(uint32_t loc, int16_t rel)
{
	return loc + 4 + (int32_t) rel * 4;
}

static uint32_t abs_jump(uint32_t loc, uint32_t imm26)
{
	return ((loc + 4) & UINT32_C(0xF0000000)) | (imm26 << 2);
}

/*
 * Disassembles a MIPS32 instruction. Both the shortest form accepted by an
 * assembler and the canonical representation of the instruction are output.
 * shrt and canon are both expected to point to 64-byte buffers.
 * If the instruction is not recognised, the 32-bit value encoding the
 * instruction is output in upper-case hexadecimal form between angle brackets
 * ('<', '>') to 'canon', and "?" is output to 'shrt'.
 */
static void disassemble_mips32(uint32_t op, uint32_t loc)
{
	unsigned int op_maj = (op >> 26) & 0x3F;
	bool valid = false;
	switch (op_maj) {
	case 0x00:  // SPECIAL prefix
		valid = disassemble_mips32_special(op, loc);
		break;
	case 0x01:  // REGIMM prefix
		valid = disassemble_mips32_regimm(op, loc);
		break;
	case 0x04:  // I-type
	case 0x05:
	case 0x06:
	case 0x07:
	case 0x08:
	case 0x09:
	case 0x0A:
	case 0x0B:
	case 0x0C:
	case 0x0D:
	case 0x0E:
	case 0x0F:
	case 0x14:
	case 0x15:
	case 0x16:
	case 0x17:
	case 0x20:
	case 0x21:
	case 0x22:
	case 0x23:
	case 0x24:
	case 0x25:
	case 0x26:
	case 0x28:
	case 0x29:
	case 0x2A:
	case 0x2B:
	case 0x2E:
	case 0x30:
	case 0x38:
		valid = disassemble_mips32_i(op, loc);
		break;
	case 0x02:  // J-type
	case 0x03:
		valid = disassemble_mips32_j(op, loc);
		break;
	case 0x10:  // Coprocessor 0
		valid = disassemble_mips32_cop0(op, loc);
		break;
	case 0x1C:  // SPECIAL2 prefix
		valid = disassemble_mips32_special2(op, loc);
		break;
	case 0x2F:
		valid = disassemble_mips32_cache(op, loc);
		break;
	case 0x33:
		valid = disassemble_mips32_pref(op, loc);
		break;
	}
	if (!valid) {
		printf("?       <%08" PRIX32 ">", op);
	}
}

/*
 * Disassembles a MIPS32 SPECIAL prefix instruction.
 * Returns true and fills both buffers if the instruction is defined.
 * Returns false if the instruction is undefined.
 */
static bool disassemble_mips32_special(uint32_t op, uint32_t loc)
{
	unsigned int s = (op >> 21) & 0x1F,
		t = (op >> 16) & 0x1F,
		d = (op >> 11) & 0x1F,
		sa = (op >> 6) & 0x1F,
		op_min = op & 0x3F;
	switch (op_min) {
	case 0x00:  // SLL RD, RT, SA
		if (d == 0)  // Effectively a nop
			printf("nop");
#if defined UNLIKELY_SHORT_FORMS
		else if (t == 0)  // Moving zero into RD
			printf("li      $%u, 0", d);
		else if (sa == 0)  // Moving RT into RD
			printf("move    $%u, $%u", d, t);
#endif
		else
			printf("sll     $%u, $%u, %u", d, t, sa);
		return true;
	case 0x02:  // SRL RD, RT, SA
#if defined UNLIKELY_SHORT_FORMS
		if (d == 0)  // Effectively a nop
			printf("nop");
		else if (t == 0)  // Moving zero into RD
			printf("li      $%u, 0", d);
		else if (sa == 0)  // Moving RT into RD
			printf("move    $%u, $%u", d, t);
		else
#endif
			printf("srl     $%u, $%u, %u", d, t, sa);
		return true;
	case 0x03:  // SRA RD, RT, SA
#if defined UNLIKELY_SHORT_FORMS
		if (d == 0)  // Effectively a nop
			printf("nop");
		else if (t == 0)  // Moving zero into RD
			printf("li      $%u, 0", d);
		else if (sa == 0)  // Moving RT into RD
			printf("move    $%u, $%u", d, t);
		else
#endif
			printf("sra     $%u, $%u, %u", d, t, sa);
		return true;
	case 0x04:  // SLLV RD, RT, RS
#if defined UNLIKELY_SHORT_FORMS
		if (d == 0)  // Effectively a nop
			printf("nop");
		else if (t == 0)  // Moving zero into RD
			printf("li      $%u, 0", d);
		else if (s == 0)  // Moving RT into RD
			printf("move    $%u, $%u", d, t);
		else
#endif
			printf("sllv    $%u, $%u, $%u", d, t, s);
		return true;
	case 0x06:  // SRLV RD, RT, SA
#if defined UNLIKELY_SHORT_FORMS
		if (d == 0)  // Effectively a nop
			printf("nop");
		else if (t == 0)  // Moving zero into RD
			printf("li      $%u, 0", d);
		else if (s == 0)  // Moving RT into RD
			printf("move    $%u, $%u", d, t);
		else
#endif
			printf("srlv    $%u, $%u, $%u", d, t, s);
		return true;
	case 0x07:  // SRAV RD, RT, SA
#if defined UNLIKELY_SHORT_FORMS
		if (d == 0)  // Effectively a nop
			printf("nop");
		else if (t == 0)  // Moving zero into RD
			printf("li      $%u, 0", d);
		else if (s == 0)  // Moving RT into RD
			printf("move    $%u, $%u", d, t);
		else
#endif
			printf("srav    $%u, $%u, $%u", d, t, s);
		return true;
	case 0x08:  // JR RS
		printf("jr      $%u", s);
		return true;
	case 0x09:  // JALR RD, RS
		printf("jalr    $%u, $%u", d, s);
		return true;
	case 0x0A:  // MOVZ RD, RS, RT
#if defined UNLIKELY_SHORT_FORMS
		if (d == 0 || d == s)  // Effectively a nop
			printf("nop");
		else if (t == 0)  // Moving RS into RD
			printf("move    $%u, $%u", d, s);
		else
#endif
			printf("movz    $%u, $%u, $%u", d, s, t);
		return true;
	case 0x0B:  // MOVN RD, RS, RT
#if defined UNLIKELY_SHORT_FORMS
		if (d == 0 || t == 0 || d == s)  // Effectively a nop
			printf("nop");
		else
#endif
			printf("movn    $%u, $%u, $%u", d, s, t);
		return true;
	case 0x0C:  // SYSCALL imm20
		printf("syscall 0x%" PRIX32, (op >> 6) & UINT32_C(0xFFFFF));
		return true;
	case 0x0D:  // BREAK imm20
		printf("break   0x%" PRIX32, (op >> 6) & UINT32_C(0xFFFFF));
		return true;
	case 0x0F:  // SYNC stype (the same field as sa)
		printf("sync    0x%X", sa);
		return true;
	case 0x10:  // MFHI RD
#if defined UNLIKELY_SHORT_FORMS
		if (d == 0)  // Effectively a nop
			printf("nop");
		else
#endif
			printf("mfhi    $%u", d);
		return true;
	case 0x11:  // MTHI RS
		printf("mthi    $%u", s);
		return true;
	case 0x12:  // MFLO RD
#if defined UNLIKELY_SHORT_FORMS
		if (d == 0)  // Effectively a nop
			printf("nop");
		else
#endif
			printf("mflo    $%u", d);
		return true;
	case 0x13:  // MTLO RS
		printf("mtlo    $%u", s);
		return true;
	case 0x18:  // MULT RS, RT
		printf("mult    $%u, $%u", s, t);
		return true;
	case 0x19:  // MULTU RS, RT
		printf("multu   $%u, $%u", s, t);
		return true;
	case 0x1A:  // DIV RS, RT
		printf("div     $%u, $%u", s, t);
		return true;
	case 0x1B:  // DIVU RS, RT
		printf("divu    $%u, $%u", s, t);
		return true;
	case 0x20:  // ADD RD, RS, RT
#if defined UNLIKELY_SHORT_FORMS
		if (d == 0 || (d == s && s != 0 && t == 0)
		 || (d == t && t != 0 && s == 0))  // Effectively a nop
			printf("nop");
		else if (s == 0 && t == 0)  // Moving zero into RD
			printf("li      $%u, 0", d);
		else if (s == 0)  // Moving RT into RD
			printf("move    $%u, $%u", d, t);
		else if (t == 0)  // Moving RS into RD
			printf("move    $%u, $%u", d, s);
		else
#endif
			printf("add     $%u, $%u, $%u", d, s, t);
		return true;
	case 0x21:  // ADDU RD, RS, RT
#if defined UNLIKELY_SHORT_FORMS
		if (d == 0 || (d == s && s != 0 && t == 0)
		 || (d == t && t != 0 && s == 0))  // Effectively a nop
			printf("nop");
		else if (s == 0 && t == 0)  // Moving zero into RD
			printf("li      $%u, 0", d);
		else
#endif
		if (s == 0)  // Moving RT into RD
			printf("move    $%u, $%u", d, t);
		else if (t == 0)  // Moving RS into RD
			printf("move    $%u, $%u", d, s);
		else
			printf("addu    $%u, $%u, $%u", d, s, t);
		return true;
	case 0x22:  // SUB RD, RS, RT
#if defined UNLIKELY_SHORT_FORMS
		if (d == 0 || (d == s && s != 0 && t == 0))  // Effectively a nop
			printf("nop");
		else if (s == t)  // Moving zero into RD
			printf("li      $%u, 0", d);
		else if (t == 0)  // Moving RS into RD
			printf("move    $%u, $%u", d, s);
		else
#endif
		if (s == 0)  // Negating RT into RD
			printf("negu    $%u, $%u", d, t);
		else
			printf("sub     $%u, $%u, $%u", d, s, t);
		return true;
	case 0x23:  // SUBU RD, RS, RT
#if defined UNLIKELY_SHORT_FORMS
		if (d == 0 || (d == s && s != 0 && t == 0))  // Effectively a nop
			printf("nop");
		else if (s == t)  // Moving zero into RD
			printf("li      $%u, 0", d);
		else if (t == 0)  // Moving RS into RD
			printf("move    $%u, $%u", d, s);
		else
#endif
		if (s == 0)  // Negating RT into RD
			printf("negu    $%u, $%u", d, t);
		else
			printf("subu    $%u, $%u, $%u", d, s, t);
		return true;
	case 0x24:  // AND RD, RS, RT
#if defined UNLIKELY_SHORT_FORMS
		if (d == 0 || (d == s && s == t))  // Effectively a nop
			printf("nop");
		else if (s == 0 || t == 0)  // Moving zero into RD
			printf("li      $%u, 0", d);
		else if (s == t)  // Moving RS (== RT) into RD
			printf("move    $%u, $%u", d, s);
		else
#endif
			printf("and     $%u, $%u, $%u", d, s, t);
		return true;
	case 0x25:  // OR RD, RS, RT
#if defined UNLIKELY_SHORT_FORMS
		if (d == 0 || (d == s && (t == s || t == 0))
		 || (d == t && (s == t || s == 0)))  // Effectively a nop
			printf("nop");
		else
#endif
		if (s == t) {  // Moving RS (== RT) into RD
			if (s == 0)  // Moving zero into RD
				printf("li      $%u, 0", d);
			else
				printf("move    $%u, $%u", d, s);
		} else if (s == 0 /* && t != 0 */)  // Moving RT into RD
			printf("move    $%u, $%u", d, t);
		else if (t == 0 /* && s != 0 */)  // Moving RS into RD
			printf("move    $%u, $%u", d, s);
		else
			printf("or      $%u, $%u, $%u", d, s, t);
		return true;
	case 0x26:  // XOR RD, RS, RT
#if defined UNLIKELY_SHORT_FORMS
		if (d == 0 || (d == s && t == 0) || (d == t && s == 0))  // Effectively a nop
			printf("nop");
		else if (s == t)  // Moving zero into RD
			printf("li      $%u, 0", d);
		else
#endif
			printf("xor     $%u, $%u, $%u", d, s, t);
		return true;
	case 0x27:  // NOR RD, RS, RT
#if defined UNLIKELY_SHORT_FORMS
		if (d == 0)  // Effectively a nop
			printf("nop");
		else
#endif
		if (s == 0 && t == 0)  // Loading -1 into RD
			printf("li      $%u, -1", d);
		else
			printf("nor     $%u, $%u, $%u", d, s, t);
		return true;
	case 0x2A:  // SLT RD, RS, RT
#if defined UNLIKELY_SHORT_FORMS
		if (d == 0)  // Effectively a nop
			printf("nop");
		else if (s == t)  // Moving zero into RD (Reg < Reg: 0)
			printf("li      $%u, 0", d);
		else
#endif
			printf("slt     $%u, $%u, $%u", d, s, t);
		return true;
	case 0x2B:  // SLTU RD, RS, RT
#if defined UNLIKELY_SHORT_FORMS
		if (d == 0)  // Effectively a nop
			printf("nop");
		else if (s == t)  // Moving zero into RD (Reg < Reg: 0)
			printf("li      $%u, 0", d);
		else
#endif
			printf("sltu    $%u, $%u, $%u", d, s, t);
		return true;
	case 0x30:  // TGE RS, RT
		printf("tge     $%u, $%u", s, t);
		return true;
	case 0x31:  // TGEU RS, RT
		printf("tgeu    $%u, $%u", s, t);
		return true;
	case 0x32:  // TLT RS, RT
		printf("tlt     $%u, $%u", s, t);
		return true;
	case 0x33:  // TLTU RS, RT
		printf("tltu    $%u, $%u", s, t);
		return true;
	case 0x34:  // TEQ RS, RT
		printf("teq     $%u, $%u", s, t);
		return true;
	case 0x36:  // TNE RS, RT
		printf("tne     $%u, $%u", s, t);
		return true;
	default:
		return false;
	}
}

/*
 * Disassembles a MIPS32 REGIMM prefix instruction.
 * Returns true and fills both buffers if the instruction is defined.
 * Returns false if the instruction is undefined.
 */
static bool disassemble_mips32_regimm(uint32_t op, uint32_t loc)
{
	unsigned int s = (op >> 21) & 0x1F,
		op_min = (op >> 16) & 0x1F;
	uint16_t imm = op;
	switch (op_min) {
	case 0x00:  // BLTZ RS, PC+IMM+4
#if defined UNLIKELY_SHORT_FORMS
		if (s == 0)  // Effectively a nop with a branch delay slot
			printf("nop");
		else
#endif
			printf("bltz    $%u, 0x%08" PRIX32, s, rel_jump(loc, (int16_t) imm));
		return true;
	case 0x01:  // BGEZ RS, PC+IMM+4
		if (s == 0)  // Effectively an unconditional immediate jump
			printf("b       %" PRIX32, rel_jump(loc, (int16_t) imm));
		else
			printf("bgez    $%u, 0x%08" PRIX32, s, rel_jump(loc, (int16_t) imm));
		return true;
	case 0x02:  // BLTZL RS, PC+IMM+4
		printf("bltzl   $%u, 0x%08" PRIX32, s, rel_jump(loc, (int16_t) imm));
		return true;
	case 0x03:  // BGEZL RS, PC+IMM+4
#if defined UNLIKELY_SHORT_FORMS
		if (s == 0)  // Effectively an unconditional immediate jump
			printf("b       %" PRIX32, rel_jump(loc, (int16_t) imm));
		else
#endif
			printf("bgezl   $%u, 0x%08" PRIX32, s, rel_jump(loc, (int16_t) imm));
		return true;
	case 0x08:  // TGEI RS, IMM
		printf("tgei    $%u, %" PRIi16, s, (int16_t) imm);
		return true;
	case 0x09:  // TGEIU RS, IMM
		printf("tgeiu   $%u, %" PRIu16, s, imm);
		return true;
	case 0x0A:  // TLTI RS, IMM
		printf("tlti    $%u, %" PRIi16, s, (int16_t) imm);
		return true;
	case 0x0B:  // TLTIU RS, IMM
		printf("tltiu   $%u, %" PRIu16, s, imm);
		return true;
	case 0x0C:  // TEQI RS, IMM
		printf("teqi    $%u, %" PRIi16, s, (int16_t) imm);
		return true;
	case 0x0E:  // TNEI RS, IMM
		printf("tnei    $%u, %" PRIi16, s, (int16_t) imm);
		return true;
	case 0x10:  // BLTZAL RS, PC+IMM+4
		printf("bltzal  $%u, 0x%08" PRIX32, s, rel_jump(loc, (int16_t) imm));
		return true;
	case 0x11:  // BGEZAL RS, PC+IMM+4
		if (s == 0)  // Effectively an unconditional immediate call
			printf("bal     0x%08" PRIX32, rel_jump(loc, (int16_t) imm));
		else
			printf("bgezal  $%u, 0x%08" PRIX32, s, rel_jump(loc, (int16_t) imm));
		return true;
	case 0x12:  // BLTZALL RS, PC+IMM+4
		printf("bltzall $%u, 0x%08" PRIX32, s, rel_jump(loc, (int16_t) imm));
		return true;
	case 0x13:  // BGEZALL RS, PC+IMM+4
#if defined UNLIKELY_SHORT_FORMS
		if (s == 0)  // Effectively an unconditional immediate call
			printf("bal     0x%08" PRIX32, rel_jump(loc, (int16_t) imm));
		else
#endif
			printf("bgezall $%u, 0x%08" PRIX32, s, rel_jump(loc, (int16_t) imm));
		return true;
	default:
		return false;
	}
}

/*
 * Disassembles a MIPS32 Coprocessor 0 prefix instruction.
 * Returns true and fills both buffers if the instruction is defined.
 * Returns false if the instruction is undefined.
 */
static bool disassemble_mips32_cop0(uint32_t op, uint32_t loc)
{
	if (op & UINT32_C(0x02000000)) { /* Bit 25, CO, is set */
		unsigned int op_min = op & 0x3F;
		switch (op_min) {
		case 0x01:  // TLBR
			printf("tlbr");
			return true;
		case 0x02:  // TLBWI
			printf("tlbwi");
			return true;
		case 0x06:  // TLBWR
			printf("tlbwr");
			return true;
		case 0x08:  // TLBP
			printf("tlbp");
			return true;
		case 0x18:  // ERET
			printf("eret");
			return true;
		case 0x1F:  // DERET
			printf("deret");
			return true;
		case 0x20:  // WAIT
			printf("wait");
			return true;
		default:
			return false;
		}
	} else {
		unsigned int cop0_min = (op >> 21) & 0x1F;
		unsigned int t = (op >> 16) & 0x1F,
			d = (op >> 11) & 0x1F,
			sel = op & 0x7;
		switch (cop0_min) {
		case 0x00:  // MFC0 rt, rd, sel
			printf("mfc0    $%u, $%u, %u", t, d, sel);
			return true;
		case 0x04:  // MTC0 rt, rd, sel
			printf("mtc0    $%u, $%u, %u", t, d, sel);
			return true;
		default:
			return false;
		}
	}
}

/*
 * Disassembles a MIPS32 PREF instruction.
 * Returns true and fills both buffers.
 */
static bool disassemble_mips32_pref(uint32_t op, uint32_t loc)
{
	unsigned int hint = (op >> 16) & 0x1F,
		base = (op >> 21) & 0x1F;
	uint16_t imm = op;
	printf("pref    0x%X, %" PRIi16 "($%u)", hint, (int16_t) imm, base);
	return true;
}

/*
 * Disassembles a MIPS32 CACHE instruction.
 * Returns true and fills both buffers.
 */
static bool disassemble_mips32_cache(uint32_t op, uint32_t loc)
{
	unsigned int cache_op = (op >> 16) & 0x1F,
		base = (op >> 21) & 0x1F;
	uint16_t imm = op;
	printf("cache   0x%X, %" PRIi16 "($%u)", cache_op, (int16_t) imm, base);
	return true;
}

/*
 * Disassembles a MIPS32 I-type instruction.
 * Returns true and fills both buffers if the instruction is defined.
 * Returns false if the instruction is undefined.
 */
static bool disassemble_mips32_i(uint32_t op, uint32_t loc)
{
	unsigned int op_maj = (op >> 26) & 0x3F;
	unsigned int s = (op >> 21) & 0x1F,
		t = (op >> 16) & 0x1F;
	uint16_t imm = op;
	switch (op_maj) {
	case 0x04:  // BEQ RS, RT, PC+IMM+4
		if (s == t)  // Effectively an unconditional immediate jump
			printf("b       0x%08" PRIX32, rel_jump(loc, (int16_t) imm));
		else
			printf("beq     $%u, $%u, 0x%08" PRIX32, s, t, rel_jump(loc, (int16_t) imm));
		return true;
	case 0x05:  // BNE RS, RT, PC+IMM+4
#if defined UNLIKELY_SHORT_FORMS
		if (s == t)  // Effectively a nop with a branch delay slot
			printf("nop");
		else
#endif
			printf("bne     $%u, $%u, 0x%08" PRIX32, s, t, rel_jump(loc, (int16_t) imm));
		return true;
	case 0x06:  // BLEZ RS, PC+IMM+4
		if (s == 0)  // Effectively an unconditional immediate jump
			printf("b       0x%08" PRIX32, rel_jump(loc, (int16_t) imm));
		else
			printf("blez    $%u, 0x%08" PRIX32, s, rel_jump(loc, (int16_t) imm));
		return true;
	case 0x07:  // BGTZ RS, PC+IMM+4
#if defined UNLIKELY_SHORT_FORMS
		if (s == 0)  // Effectively a nop with a branch delay slot
			printf("nop");
		else
#endif
			printf("bgtz    $%u, 0x%08" PRIX32, s, rel_jump(loc, (int16_t) imm));
		return true;
	case 0x08:  // ADDI RT, RS, IMM
#if defined UNLIKELY_SHORT_FORMS
		if (t == 0)  // Effectively a nop
			printf("nop");
		else if (s == 0)  // Loading [-32768, 32767] into RT
			printf("li      $%u, %" PRIi16, t, (int16_t) imm);
		else if (imm == 0)  // Moving RS into RT
			printf("move    $%u, $%u", t, s);
		else
#endif
			printf("addi    $%u, $%u, %" PRIi16, t, s, (int16_t) imm);
		return true;
	case 0x09:  // ADDIU RT, RS, IMM
#if defined UNLIKELY_SHORT_FORMS
		if (t == 0)  // Effectively a nop
			printf("nop");
		else
#endif
		if (s == 0)  // Loading [-32768, 32767] into RT
			printf("li      $%u, %" PRIi16, t, (int16_t) imm);
#if defined UNLIKELY_SHORT_FORMS
		else if (imm == 0)  // Moving RS into RT
			printf("move    $%u, $%u", t, s);
#endif
		else
			printf("addiu   $%u, $%u, %" PRIi16, t, s, (int16_t) imm);
		return true;
	case 0x0A:  // SLTI RT, RS, IMM
#if defined UNLIKELY_SHORT_FORMS
		if (t == 0)  // Effectively a nop
			printf("nop");
		else if (s == 0 && imm == 0)  // Moving zero into RT
			printf("li      $%u, 0", t);
		else
#endif
			printf("slti    $%u, $%u, %" PRIi16, t, s, (int16_t) imm);
		return true;
	case 0x0B:  // SLTIU RT, RS, IMM
#if defined UNLIKELY_SHORT_FORMS
		if (t == 0)  // Effectively a nop
			printf("nop");
		else if (s == 0 && imm == 0)  // Moving zero into RT
			printf("li      $%u, 0", t);
		else
#endif
			printf("sltiu   $%u, $%u, %" PRIi16, t, s, (int16_t) imm);
		return true;
	case 0x0C:  // ANDI RT, RS, IMM
#if defined UNLIKELY_SHORT_FORMS
		if (t == 0)  // Effectively a nop
			printf("nop");
		else if (imm == 0)  // Moving zero into RT
			printf("li      $%u, 0", t);
		else
#endif
			printf("andi    $%u, $%u, 0x%" PRIX16, t, s, imm);
		return true;
	case 0x0D:  // ORI RT, RS, IMM
#if defined UNLIKELY_SHORT_FORMS
		if (t == 0 || (s == t && imm == 0))  // Effectively a nop
			printf("nop");
		else
#endif
		if (s == 0 && imm > 0)  // Loading [1, 65535] into RT
			printf("li      $%u, %" PRIu16, t, imm);
#if defined UNLIKELY_SHORT_FORMS
		else if (imm == 0)  // Moving RS into RT
			printf("move    $%u, $%u", t, s);
		else
#endif
			printf("ori     $%u, $%u, 0x%" PRIX16, t, s, imm);
		return true;
	case 0x0E:  // XORI RT, RS, IMM
#if defined UNLIKELY_SHORT_FORMS
		if (t == 0 || (s == t && imm == 0))  // Effectively a nop
			printf("nop");
		else if (s == 0 && imm >= 0)  // Loading [0, 32767] into RT
			printf("li      $%u, %" PRIi16, t, (int16_t) imm);
		else if (imm == 0)  // Moving RS into RT
			printf("move    $%u, $%u", t, s);
		else
#endif
			printf("xori    $%u, $%u, 0x%" PRIX16, t, s, imm);
		return true;
	case 0x0F:  // LUI RT, IMM
#if defined UNLIKELY_SHORT_FORMS
		if (t == 0)  // Effectively a nop
			printf("nop");
		else
#endif
			printf("lui     $%u, %" PRIi16, t, imm);
		return true;
	case 0x14:  // BEQL RS, RT, PC+IMM+4
#if defined UNLIKELY_SHORT_FORMS
		if (s == t)  // Effectively an unconditional immediate jump
			printf("b       0x%08" PRIX32, rel_jump(loc, (int16_t) imm));
		else
#endif
			printf("beql    $%u, $%u, 0x%08" PRIX32, s, t, rel_jump(loc, (int16_t) imm));
		return true;
	case 0x15:  // BNEL RS, RT, PC+IMM+4
		printf("bnel    $%u, $%u, 0x%08" PRIX32, s, t, rel_jump(loc, (int16_t) imm));
		return true;
	case 0x16:  // BLEZL RS, PC+IMM+4
#if defined UNLIKELY_SHORT_FORMS
		if (s == 0)  // Effectively an unconditional immediate jump
			printf("b       0x%08" PRIX32, rel_jump(loc, (int16_t) imm));
		else
#endif
			printf("blezl   $%u, 0x%08" PRIX32, s, rel_jump(loc, (int16_t) imm));
		return true;
	case 0x17:  // BGTZL RS, PC+IMM+4
		printf("bgtzl   $%u, 0x%08" PRIX32, s, rel_jump(loc, (int16_t) imm));
		return true;
	case 0x20:  // LB RT, IMM(RS)
		printf("lb      $%u, %" PRIi16 "($%u)", t, (int16_t) imm, s);
		return true;
	case 0x21:  // LH RT, IMM(RS)
		printf("lh      $%u, %" PRIi16 "($%u)", t, (int16_t) imm, s);
		return true;
	case 0x22:  // LWL RT, IMM(RS)
		printf("lwl     $%u, %" PRIi16 "($%u)", t, (int16_t) imm, s);
		return true;
	case 0x23:  // LW RT, IMM(RS)
		printf("lw      $%u, %" PRIi16 "($%u)", t, (int16_t) imm, s);
		return true;
	case 0x24:  // LBU RT, IMM(RS)
		printf("lbu     $%u, %" PRIi16 "($%u)", t, (int16_t) imm, s);
		return true;
	case 0x25:  // LHU RT, IMM(RS)
		printf("lhu     $%u, %" PRIi16 "($%u)", t, (int16_t) imm, s);
		return true;
	case 0x26:  // LWR RT, IMM(RS)
		printf("lwr     $%u, %" PRIi16 "($%u)", t, (int16_t) imm, s);
		return true;
	case 0x28:  // SB RT, IMM(RS)
		printf("sb      $%u, %" PRIi16 "($%u)", t, (int16_t) imm, s);
		return true;
	case 0x29:  // SH RT, IMM(RS)
		printf("sh      $%u, %" PRIi16 "($%u)", t, (int16_t) imm, s);
		return true;
	case 0x2A:  // SWL RT, IMM(RS)
		printf("swl     $%u, %" PRIi16 "($%u)", t, (int16_t) imm, s);
		return true;
	case 0x2B:  // SW RT, IMM(RS)
		printf("sw      $%u, %" PRIi16 "($%u)", t, (int16_t) imm, s);
		return true;
	case 0x2E:  // SWR RT, IMM(RS)
		printf("swr     $%u, %" PRIi16 "($%u)", t, (int16_t) imm, s);
		return true;
	case 0x30:  // LL RT, IMM(RS)
		printf("ll      $%u, %" PRIi16 "($%u)", t, (int16_t) imm, s);
		return true;
	case 0x38:  // SC RT, IMM(RS)
		printf("sc      $%u, %" PRIi16 "($%u)", t, (int16_t) imm, s);
		return true;
	default:
		return false;
	}
}

/*
 * Disassembles a MIPS32 SPECIAL2 prefix instruction.
 * Returns true and fills both buffers if the instruction is defined.
 * Returns false if the instruction is undefined.
 */
static bool disassemble_mips32_special2(uint32_t op, uint32_t loc)
{
	unsigned int s = (op >> 21) & 0x1F,
		t = (op >> 16) & 0x1F,
		d = (op >> 11) & 0x1F,
		op_min = op & 0x3F;
	switch (op_min) {
	case 0x00:  // MADD RS, RT
#if defined UNLIKELY_SHORT_FORMS
		if (s == 0 || t == 0)  // Effectively a nop
			printf("nop");
		else
#endif
			printf("madd    $%u, $%u", s, t);
		return true;
	case 0x01:  // MADDU RS, RT
#if defined UNLIKELY_SHORT_FORMS
		if (s == 0 || t == 0)  // Effectively a nop
			printf("nop");
		else
#endif
			printf("maddu   $%u, $%u", s, t);
		return true;
	case 0x02:  // MUL RD, RS, RT
#if defined UNLIKELY_SHORT_FORMS
		if (d == 0)  // Effectively a nop
			printf("nop");
		else if (s == 0 || t == 0)  // Moving zero into RD
			printf("li      $%u, 0", d);
		else
#endif
			printf("mul     $%u, $%u, $%u", d, s, t);
		return true;
	case 0x04:  // MSUB RS, RT
#if defined UNLIKELY_SHORT_FORMS
		if (s == 0 || t == 0)  // Effectively a nop
			printf("nop");
		else
#endif
			printf("msub    $%u, $%u", s, t);
		return true;
	case 0x05:  // MSUBU RS, RT
#if defined UNLIKELY_SHORT_FORMS
		if (s == 0 || t == 0)  // Effectively a nop
			printf("nop");
		else
#endif
			printf("msubu   $%u, $%u", s, t);
		return true;
	case 0x20:  // CLZ RD, RS
#if defined UNLIKELY_SHORT_FORMS
		if (d == 0)  // Effectively a nop
			printf("nop");
		else
#endif
			printf("clz     $%u, $%u", d, s);
		return true;
	case 0x21:  // CLO RD, RS
#if defined UNLIKELY_SHORT_FORMS
		if (d == 0)  // Effectively a nop
			printf("nop");
		else
#endif
			printf("clo     $%u, $%u", d, s);
		return true;
	case 0x3F:  // SDBBP imm20
		printf("sdbbp   0x%" PRIX32, (op >> 6) & UINT32_C(0xFFFFF));
		return true;
	default:
		return false;
	}
}

/*
 * Disassembles a MIPS32 J-type instruction.
 * Returns true and fills both buffers if the instruction is defined.
 * Returns false if the instruction is undefined.
 */
static bool disassemble_mips32_j(uint32_t op, uint32_t loc)
{
	unsigned int op_maj = (op >> 26) & 0x3F;
	uint32_t imm = (op >> 6) & UINT32_C(0x03FFFFFF);
	switch (op_maj) {
	case 0x02:  // J PCSeg4.IMM26.00
		printf("j       0x%08" PRIX32, abs_jump(loc, imm));
		return true;
	case 0x03:  // JAL PCSeg4.IMM26.00
		printf("jal     0x%08" PRIX32, abs_jump(loc, imm));
		return true;
	default:
		return false;
	}
}

void process_exception()
{
	struct card_reply_mips_exception exception;
	size_t i;

	REG_IME = IME_ENABLE;
	card_read_data(sizeof(exception), &exception, true);
	REG_IME = IME_DISABLE;

	set_sub_text();
	consoleClear();
	printf("    - Supercard exception -\n%02" PRIX32 " %s\nat address %08" PRIX32 "\n\n",
	       exception.excode, excode_desc(exception.excode), exception.c0_epc);

	if (exception.mapped) {
		printf("-> ");
		disassemble_mips32(exception.op, exception.c0_epc);
		printf("\n   ");
		disassemble_mips32(exception.next_op, exception.c0_epc + 4);
		printf("\n\n");
	} else if ((exception.c0_epc & 3) != 0) {
		printf("     (%08" PRIX32 " is unaligned)\n\n\n", exception.c0_epc);
	} else {
		printf("     (%08" PRIX32 " is unmapped)\n\n\n", exception.c0_epc);
	}

	for (i = 0; i < REG_COUNT; i++) {
		printf("  %2" PRIu8 " = %08" PRIX32, reg_nums[i],
		       *(uint32_t*) ((uint8_t*) &exception + reg_offs[i]));
		if (i & 1) {
			printf("\n");
		}
	}

	if (REG_COUNT & 1) {
		printf("\n");
	}

	printf("  HI = %08" PRIX32 "  LO = %08" PRIX32, exception.hi, exception.lo);

	link_status = LINK_STATUS_ERROR;
}
