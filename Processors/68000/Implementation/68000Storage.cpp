//
//  68000Storage.cpp
//  Clock Signal
//
//  Created by Thomas Harte on 08/03/2019.
//  Copyright © 2019 Thomas Harte. All rights reserved.
//

#include "../68000.hpp"

#include <algorithm>
#include <sstream>

namespace CPU {
namespace MC68000 {

#define Dn		0x00
#define An		0x01
#define Ind		0x02
#define	PostInc	0x03
#define PreDec	0x04
#define d16An	0x05
#define d8AnXn	0x06
#define XXXw	0x10
#define XXXl	0x11
#define d16PC	0x12
#define d8PCXn	0x13
#define Imm		0x14

struct ProcessorStorageConstructor {
	ProcessorStorageConstructor(ProcessorStorage &storage) : storage_(storage) {}

	using BusStep = ProcessorStorage::BusStep;

	/*!
	*/
	int calc_action_for_mode(int mode) const {
		using Action = ProcessorBase::MicroOp::Action;
		switch(mode & 0xff) {
			default: assert(false);
			case d16PC:		return int(Action::CalcD16PC);
			case d8PCXn:	return int(Action::CalcD8PCXn);
			case d16An:		return int(Action::CalcD16An);
			case d8AnXn:	return int(Action::CalcD8AnXn);
		}
	}

	int address_assemble_for_mode(int mode) const {
		using Action = ProcessorBase::MicroOp::Action;
		assert((mode & 0xff) == XXXw || (mode & 0xff) == XXXl);
		return int(((mode & 0xff) == XXXw) ? Action::AssembleWordAddressFromPrefetch : Action::AssembleLongWordAddressFromPrefetch);
	}

	int address_action_for_mode(int mode) const {
		using Action = ProcessorBase::MicroOp::Action;
		switch(mode & 0xff) {
			default: assert(false);
			case d16PC:		return int(Action::CalcD16PC);
			case d8PCXn:	return int(Action::CalcD8PCXn);
			case d16An:		return int(Action::CalcD16An);
			case d8AnXn:	return int(Action::CalcD8AnXn);
			case XXXw:		return int(Action::AssembleWordAddressFromPrefetch);
			case XXXl:		return int(Action::AssembleLongWordAddressFromPrefetch);
		}
	}

	int combined_mode(int mode, int reg, bool collapse_an_dn = false, bool collapse_postinc = false) {
		if(collapse_an_dn && mode == An) mode = Dn;
		if(collapse_postinc && mode == PostInc) mode = Ind;
		return (mode == 7) ? (0x10 | reg) : mode;
	}

	int data_assemble_for_mode(int mode) const {
		using Action = ProcessorBase::MicroOp::Action;
		assert((mode & 0xff) == XXXw || (mode & 0xff) == XXXl);
		return int(((mode & 0xff) == XXXw) ? Action::AssembleWordDataFromPrefetch : Action::AssembleLongWordDataFromPrefetch);
	}

	int byte_inc(int reg) const {
		using Action = ProcessorBase::MicroOp::Action;
		// Special case: stack pointer byte accesses adjust by two.
		return int((reg == 7) ? Action::Increment2 : Action::Increment1);
	}

	int byte_dec(int reg) const {
		using Action = ProcessorBase::MicroOp::Action;
		// Special case: stack pointer byte accesses adjust by two.
		return int((reg == 7) ? Action::Decrement2 : Action::Decrement1);
	}

	int increment_action(bool is_long_word_access, bool is_byte_access, int reg) const {
		using Action = ProcessorBase::MicroOp::Action;
		if(is_long_word_access) return int(Action::Increment4);
		if(is_byte_access) return byte_inc(reg);
		return int(Action::Increment2);
	}

	int decrement_action(bool is_long_word_access, bool is_byte_access, int reg) const {
		using Action = ProcessorBase::MicroOp::Action;
		if(is_long_word_access) return int(Action::Decrement4);
		if(is_byte_access) return byte_dec(reg);
		return int(Action::Decrement2);
	}

#define pseq(x, m) ((((m)&0xff) == 0x06) || (((m)&0xff) == 0x13) ? "n " x : x)

	/*!
		Installs BusSteps that implement the described program into the relevant
		instance storage, returning the offset within @c all_bus_steps_ at which
		the generated steps begin.

		@param access_pattern A string describing the bus activity that occurs
			during this program. This should follow the same general pattern as
			those in yacht.txt; full description below.

		@param addresses A vector of the addresses to place on the bus coincident
			with those acess steps that require them.

		@param read_full_words @c true to indicate that read and write operations are
			selecting a full word; @c false to signal byte accesses only.

		@discussion
		The access pattern is defined to correlate closely to that in yacht.txt; it is
		a space-separated sequence of the following actions:

		* n: no operation for four cycles; data bus is not used;
		* nn: no operation for eight cycles; data bus is not used;
		* r: a 'replaceable'-length no operation; data bus is not used and no guarantees are
			made about the length of the cycle other than that when it reaches the interpreter,
			it is safe to alter the length and leave it altered;
		* np: program fetch; reads from the PC and adds two to it, advancing the prefetch queue;
		* nW: write MSW of something onto the bus;
		* nw: write LSW of something onto the bus;
		* nR: read MSW of something from the bus into the source latch;
		* nr: read LSW of soemthing from the bus into the source latch;
		* nRd: read MSW of something from the bus into the destination latch;
		* nrd: read LSW of soemthing from the bus into the destination latch;
		* nS: push the MSW of something onto the stack **and then** decrement the pointer;
		* ns: push the LSW of something onto the stack **and then** decrement the pointer;
		* nU: pop the MSW of something from the stack;
		* nu: pop the LSW of something from the stack;
		* nV: fetch a vector's MSW;
		* nv: fetch a vector's LSW;
		* i: acquire interrupt vector in an IACK cycle;
		* nF: fetch the SSPs MSW;
		* nf: fetch the SSP's LSW;
		* _: hold the reset line active for the usual period.
		* tas: perform the final 6 cycles of a TAS: like an n nw but with the address strobe active for the entire period.

		Quite a lot of that is duplicative, implying both something about internal
		state and something about what's observable on the bus, but it's helpful to
		stick to that document's coding exactly for easier debugging.

		np fetches will fill the prefetch queue, attaching an action to both the
		step that precedes them and to themselves. The SSP fetches will go straight
		to the SSP.

		Other actions will by default act via effective_address_ and bus_data_.
		The user should fill in the steps necessary to get data into or extract
		data from those.

		nr/nw-type operations may have a + or - suffix; if such a suffix is attached
		then the corresponding effective address will be incremented or decremented
		by two after the cycle has completed.
	*/
	size_t assemble_program(std::string access_pattern, const std::vector<uint32_t *> &addresses = {}, bool read_full_words = true) {
		auto address_iterator = addresses.begin();
		using Action = BusStep::Action;

		std::vector<BusStep> steps;
		std::stringstream stream(access_pattern);

		// Tokenise the access pattern by splitting on spaces.
		std::string token;
		while(stream >> token) {
			ProcessorBase::BusStep step;

			// Check for a plus-or-minus suffix.
			int post_adjustment = 0;
			if(token.back() == '-' || token.back() == '+') {
				if(token.back() == '-') {
					post_adjustment = -1;
				}

				if(token.back() == '+') {
					post_adjustment = 1;
				}

				token.pop_back();
			}

			// Do nothing (possibly twice).
			if(token == "n" || token == "nn") {
				if(token.size() == 2) {
					step.microcycle.length = HalfCycles(8);
				}
				steps.push_back(step);
				continue;
			}

			// Do nothing, but with a length that definitely won't map it to the other do-nothings.
			if(token == "r"){
				step.microcycle.length = HalfCycles(0);
				steps.push_back(step);
				continue;
			}

			// Fetch SSP.
			if(token == "nF" || token == "nf") {
				step.microcycle.length = HalfCycles(5);
				step.microcycle.operation = Microcycle::NewAddress | Microcycle::Read | Microcycle::IsProgram;	// IsProgram is a guess.
				step.microcycle.address = &storage_.effective_address_[0].full;
				step.microcycle.value = isupper(token[1]) ? &storage_.stack_pointers_[1].halves.high : &storage_.stack_pointers_[1].halves.low;
				steps.push_back(step);

				step.microcycle.length = HalfCycles(3);
				step.microcycle.operation = Microcycle::SameAddress | Microcycle::Read | Microcycle::IsProgram | Microcycle::SelectWord;
				step.action = Action::IncrementEffectiveAddress0;
				steps.push_back(step);

				continue;
			}

			// Fetch exception vector.
			if(token == "nV" || token == "nv") {
				step.microcycle.length = HalfCycles(5);
				step.microcycle.operation = Microcycle::NewAddress | Microcycle::Read | Microcycle::IsProgram;	// IsProgram is a guess.
				step.microcycle.address = &storage_.effective_address_[0].full;
				step.microcycle.value = isupper(token[1]) ? &storage_.program_counter_.halves.high : &storage_.program_counter_.halves.low;
				steps.push_back(step);

				step.microcycle.length = HalfCycles(3);
				step.microcycle.operation = Microcycle::SameAddress | Microcycle::Read | Microcycle::IsProgram | Microcycle::SelectWord;
				step.action = Action::IncrementEffectiveAddress0;
				steps.push_back(step);

				continue;
			}

			// Fetch from the program counter into the prefetch queue.
			if(token == "np") {
				step.microcycle.length = HalfCycles(5);
				step.microcycle.operation = Microcycle::NewAddress | Microcycle::Read | Microcycle::IsProgram;
				step.microcycle.address = &storage_.program_counter_.full;
				step.microcycle.value = &storage_.prefetch_queue_.halves.low;
				step.action = Action::AdvancePrefetch;
				steps.push_back(step);

				step.microcycle.length = HalfCycles(3);
				step.microcycle.operation = Microcycle::SameAddress | Microcycle::Read | Microcycle::IsProgram | Microcycle::SelectWord;
				step.action = Action::IncrementProgramCounter;
				steps.push_back(step);

				continue;
			}

			// The reset cycle.
			if(token == "_") {
				step.microcycle.length = HalfCycles(248);
				step.microcycle.operation = Microcycle::Reset;
				steps.push_back(step);

				continue;
			}

			// A standard read or write.
			if(token == "nR" || token == "nr" || token == "nW" || token == "nw" || token == "nRd" || token == "nrd") {
				const bool is_read = tolower(token[1]) == 'r';
				const bool use_source_storage = is_read && token.size() != 3;
				RegisterPair32 *const scratch_data = use_source_storage ? &storage_.source_bus_data_[0] : &storage_.destination_bus_data_[0];

				assert(address_iterator != addresses.end());

				step.microcycle.length = HalfCycles(5);
				step.microcycle.operation = Microcycle::NewAddress | (is_read ? Microcycle::Read : 0);
				step.microcycle.address = *address_iterator;
				step.microcycle.value = isupper(token[1]) ? &scratch_data->halves.high : &scratch_data->halves.low;
				steps.push_back(step);

				step.microcycle.length = HalfCycles(3);
				step.microcycle.operation = Microcycle::SameAddress | (is_read ? Microcycle::Read : 0) | (read_full_words ? Microcycle::SelectWord : Microcycle::SelectByte);
				if(post_adjustment) {
					// nr and nR should affect address 0; nw, nW, nrd and nRd should affect address 1.
					if(tolower(token[1]) == 'r' && token.size() == 2) {
						step.action = (post_adjustment > 0) ? Action::IncrementEffectiveAddress0 : Action::DecrementEffectiveAddress0;
					} else {
						step.action = (post_adjustment > 0) ? Action::IncrementEffectiveAddress1 : Action::DecrementEffectiveAddress1;

					}
				}
				steps.push_back(step);
				++address_iterator;

				continue;
			}

			// The completing part of a TAS.
			if(token == "tas") {
				RegisterPair32 *const scratch_data = &storage_.destination_bus_data_[0];

				assert(address_iterator != addresses.end());

				step.microcycle.length = HalfCycles(9);
				step.microcycle.operation = Microcycle::SameAddress;
				step.microcycle.address = *address_iterator;
				step.microcycle.value = &scratch_data->halves.low;
				steps.push_back(step);

				step.microcycle.length = HalfCycles(3);
				step.microcycle.operation = Microcycle::SameAddress | Microcycle::SelectByte;
				steps.push_back(step);
				++address_iterator;

				continue;
			}

			// A stack write.
			if(token == "nS" || token == "ns") {
				RegisterPair32 *const scratch_data = &storage_.destination_bus_data_[0];

				step.microcycle.length = HalfCycles(5);
				step.microcycle.operation = Microcycle::NewAddress;
				step.microcycle.address = &storage_.effective_address_[1].full;
				step.microcycle.value = isupper(token[1]) ? &scratch_data->halves.high : &scratch_data->halves.low;
				steps.push_back(step);

				step.microcycle.length = HalfCycles(3);
				step.microcycle.operation = Microcycle::SameAddress | Microcycle::SelectWord;
				step.action = Action::DecrementEffectiveAddress1;
				steps.push_back(step);

				continue;
			}

			// A stack read.
			if(token == "nU" || token == "nu") {
				RegisterPair32 *const scratch_data = &storage_.source_bus_data_[0];

				step.microcycle.length = HalfCycles(5);
				step.microcycle.operation = Microcycle::NewAddress | Microcycle::Read;
				step.microcycle.address = &storage_.effective_address_[0].full;
				step.microcycle.value = isupper(token[1]) ? &scratch_data->halves.high : &scratch_data->halves.low;
				steps.push_back(step);

				step.microcycle.length = HalfCycles(3);
				step.microcycle.operation = Microcycle::SameAddress | Microcycle::Read | Microcycle::SelectWord;
				step.action = Action::IncrementEffectiveAddress0;
				steps.push_back(step);

				continue;
			}

			std::cerr << "MC68000 program builder; Unknown access token " << token << std::endl;
			assert(false);
		}

		// Add a final 'ScheduleNextProgram' sentinel.
		BusStep end_program;
		end_program.action = Action::ScheduleNextProgram;
		steps.push_back(end_program);

		// If the new steps already exist, just return the existing index to them;
		// otherwise insert them.
		const auto position = std::search(storage_.all_bus_steps_.begin(), storage_.all_bus_steps_.end(), steps.begin(), steps.end());
		if(position != storage_.all_bus_steps_.end()) {
			return size_t(position - storage_.all_bus_steps_.begin());
		}

		const auto start = storage_.all_bus_steps_.size();
		std::copy(steps.begin(), steps.end(), std::back_inserter(storage_.all_bus_steps_));
		return start;
	}

	/*!
		Disassembles the instruction @c instruction and inserts it into the
		appropriate lookup tables.

		install_instruction acts, in effect, in the manner of a disassembler. So this class is
		formulated to run through all potential 65536 instuction encodings and attempt to
		disassemble each, rather than going in the opposite direction.

		This has two benefits:

			(i) which addressing modes go with which instructions falls out automatically;
			(ii) it is a lot easier during the manual verification stage of development to work
				from known instructions to their disassembly rather than vice versa; especially
			(iii) given that there are plentiful disassemblers against which to test work in progress.
	*/
	void install_instructions() {
		enum class Decoder {
			ABCD_SBCD,					// Maps source and desintation registers and a register/memory selection bit to an ABCD or SBCD.
			ADD_SUB,					// Maps a register and a register and mode to an ADD or SUB.
			ADDA_SUBA,					// Maps a destination register and a source mode and register to an ADDA or SUBA.
			ADDQ_SUBQ,					// Maps a register and a mode to an ADDQ or SUBQ.

			AND_OR_EOR,					// Maps a source register, operation mode and destination register and mode to an AND, OR or EOR.

			BRA,						// Maps to a BRA. All fields are decoded at runtime.
			Bcc_BSR,					// Maps to a Bcc or BSR. Other than determining the type of operation, fields are decoded at runtime.

			BTST,						// Maps a source register and a destination register and mode to a BTST.
			BTSTIMM,					// Maps a destination mode and register to a BTST #.

			BCLR,						// Maps a source register and a destination register and mode to a BCLR.
			BCLRIMM,					// Maps a destination mode and register to a BCLR #.

			CLR_NEG_NEGX_NOT,			// Maps a destination mode and register to a CLR, NEG, NEGX or NOT.

			CMP,						// Maps a destination register and a source mode and register to a CMP.
			CMPI,						// Maps a destination mode and register to a CMPI.
			CMPA,						// Maps a destination register and a source mode and register to a CMPA.
			CMPM,						// Maps to a CMPM.

			EORI_ORI_ANDI_SUBI_ADDI,	// Maps a mode and register to one of EORI, ORI, ANDI, SUBI or ADDI.

			JMP,						// Maps a mode and register to a JMP.
			JSR,						// Maps a mode and register to a JSR.

			LEA,						// Maps a destination register and a source mode and register to an LEA.

			MOVE,						// Maps a source mode and register and a destination mode and register to a MOVE.
			MOVEtoSRCCR,				// Maps a source mode and register to a MOVE to SR or MOVE to CCR.
			MOVEfromSR,					// Maps a source mode and register to a MOVE fom SR.
			MOVEq,						// Maps a destination register to a MOVEQ.

			MULU_MULS,					// Maps a destination register and a source mode and register to a MULU or MULS.

			RESET,						// Maps to a RESET.

			ASLR_LSLR_ROLR_ROXLRr,		// Maps a destination register to a AS[L/R], LS[L/R], RO[L/R], ROX[L/R]; shift quantities are
										// decoded at runtime.
			ASLR_LSLR_ROLR_ROXLRm,		// Maps a destination mode and register to a memory-based AS[L/R], LS[L/R], RO[L/R], ROX[L/R].

			MOVEM,						// Maps a mode and register as they were a 'destination' and sets up bus steps with a suitable
										// hole for the runtime part to install proper MOVEM activity.

			RTE_RTR,					// Maps to an RTE/RTR.

			Scc_DBcc,					// Maps a mode and destination register to either a DBcc or Scc.

			TST,						// Maps a mode and register to a TST.

			RTS,						// Maps to an RST.

			MOVEUSP,					// Maps a direction and register to a MOVE [to/from] USP.

			TRAP,						// Maps to a TRAP.

			NOP,						// Maps to a NOP.

			EXG,						// Maps source and destination registers and an operation mode to an EXG.
			EXT_SWAP,					// Maps a source register to a SWAP or EXT.

			EORI_ORI_ANDI_SR,			// Maps to an EORI, ORI or ANDI to SR/CCR.

			BCHG_BSET,					// Maps a mode and register, and possibly a source register, to a BCHG or BSET.

			TAS,						// Maps a mode and register to a TAS.
		};

		using Operation = ProcessorStorage::Operation;
		using Action = ProcessorStorage::MicroOp::Action;
		using MicroOp = ProcessorBase::MicroOp;
		struct PatternMapping {
			uint16_t mask, value;
			Operation operation;
			Decoder decoder;
		};

		/*
			Inspired partly by 'wrm' (https://github.com/wrm-za I assume); the following
			table draws from the M68000 Programmer's Reference Manual, currently available at
			https://www.nxp.com/files-static/archives/doc/ref_manual/M68000PRM.pdf

			After each line is the internal page number on which documentation of that
			instruction mapping can be found, followed by the page number within the PDF
			linked above.

			NB: a vector is used to allow easy iteration.
		*/
		const std::vector<PatternMapping> mappings = {
			{0xf1f0, 0xc100, Operation::ABCD, Decoder::ABCD_SBCD},		// 4-3 (p107)
			{0xf1f0, 0x8100, Operation::SBCD, Decoder::ABCD_SBCD},		// 4-171 (p275)

			{0xf0c0, 0xc000, Operation::ANDb, Decoder::AND_OR_EOR},		// 4-15 (p119)
			{0xf0c0, 0xc040, Operation::ANDw, Decoder::AND_OR_EOR},		// 4-15 (p119)
			{0xf0c0, 0xc080, Operation::ANDl, Decoder::AND_OR_EOR},		// 4-15 (p119)

			{0xf0c0, 0x8000, Operation::ORb, Decoder::AND_OR_EOR},		// 4-150 (p254)
			{0xf0c0, 0x8040, Operation::ORw, Decoder::AND_OR_EOR},		// 4-150 (p254)
			{0xf0c0, 0x8080, Operation::ORl, Decoder::AND_OR_EOR},		// 4-150 (p254)

			{0xf0c0, 0xb000, Operation::EORb, Decoder::AND_OR_EOR},		// 4-100 (p204)
			{0xf0c0, 0xb040, Operation::EORw, Decoder::AND_OR_EOR},		// 4-100 (p204)
			{0xf0c0, 0xb080, Operation::EORl, Decoder::AND_OR_EOR},		// 4-100 (p204)

			{0xffc0, 0x0600, Operation::ADDb, Decoder::EORI_ORI_ANDI_SUBI_ADDI},	// 4-9 (p113)
			{0xffc0, 0x0640, Operation::ADDw, Decoder::EORI_ORI_ANDI_SUBI_ADDI},	// 4-9 (p113)
			{0xffc0, 0x0680, Operation::ADDl, Decoder::EORI_ORI_ANDI_SUBI_ADDI},	// 4-9 (p113)

			{0xffc0, 0x0200, Operation::ANDb, Decoder::EORI_ORI_ANDI_SUBI_ADDI},	// 4-9 (p113)
			{0xffc0, 0x0240, Operation::ANDw, Decoder::EORI_ORI_ANDI_SUBI_ADDI},	// 4-9 (p113)
			{0xffc0, 0x0280, Operation::ANDl, Decoder::EORI_ORI_ANDI_SUBI_ADDI},	// 4-9 (p113)

			{0xffc0, 0x0a00, Operation::ORb, Decoder::EORI_ORI_ANDI_SUBI_ADDI},		// 4-153 (p257)
			{0xffc0, 0x0a40, Operation::ORw, Decoder::EORI_ORI_ANDI_SUBI_ADDI},		// 4-153 (p257)
			{0xffc0, 0x0a80, Operation::ORl, Decoder::EORI_ORI_ANDI_SUBI_ADDI},		// 4-153 (p257)

			{0xffc0, 0x0000, Operation::EORb, Decoder::EORI_ORI_ANDI_SUBI_ADDI},	// 4-102 (p206)
			{0xffc0, 0x0040, Operation::EORw, Decoder::EORI_ORI_ANDI_SUBI_ADDI},	// 4-102 (p206)
			{0xffc0, 0x0080, Operation::EORl, Decoder::EORI_ORI_ANDI_SUBI_ADDI},	// 4-102 (p206)

			{0xffc0, 0x0400, Operation::SUBb, Decoder::EORI_ORI_ANDI_SUBI_ADDI},	// 4-179 (p283)
			{0xffc0, 0x0440, Operation::SUBw, Decoder::EORI_ORI_ANDI_SUBI_ADDI},	// 4-179 (p283)
			{0xffc0, 0x0480, Operation::SUBl, Decoder::EORI_ORI_ANDI_SUBI_ADDI},	// 4-179 (p283)

			{0xf000, 0x1000, Operation::MOVEb, Decoder::MOVE},	// 4-116 (p220)
			{0xf000, 0x2000, Operation::MOVEl, Decoder::MOVE},	// 4-116 (p220)
			{0xf000, 0x3000, Operation::MOVEw, Decoder::MOVE},	// 4-116 (p220)

			{0xffc0, 0x46c0, Operation::MOVEtoSR, Decoder::MOVEtoSRCCR},	// 6-19 (p473)
			{0xffc0, 0x44c0, Operation::MOVEtoCCR, Decoder::MOVEtoSRCCR},	// 4-123 (p227)
			{0xffc0, 0x40c0, Operation::MOVEfromSR, Decoder::MOVEfromSR},	// 6-17 (p471)

			{0xf1c0, 0xb000, Operation::CMPb, Decoder::CMP},	// 4-75 (p179)
			{0xf1c0, 0xb040, Operation::CMPw, Decoder::CMP},	// 4-75 (p179)
			{0xf1c0, 0xb080, Operation::CMPl, Decoder::CMP},	// 4-75 (p179)

			{0xf1c0, 0xb0c0, Operation::CMPw, Decoder::CMPA},	// 4-77 (p181)
			{0xf1c0, 0xb1c0, Operation::CMPl, Decoder::CMPA},	// 4-77 (p181)

			{0xffc0, 0x0c00, Operation::CMPb, Decoder::CMPI},	// 4-79 (p183)
			{0xffc0, 0x0c40, Operation::CMPw, Decoder::CMPI},	// 4-79 (p183)
			{0xffc0, 0x0c80, Operation::CMPl, Decoder::CMPI},	// 4-79 (p183)

			{0xf1f8, 0xb108, Operation::CMPb, Decoder::CMPM},	// 4-81 (p185)
			{0xf1f8, 0xb148, Operation::CMPw, Decoder::CMPM},	// 4-81 (p185)
			{0xf1f8, 0xb188, Operation::CMPl, Decoder::CMPM},	// 4-81 (p185)

//			{0xff00, 0x6000, Operation::BRA, Decoder::BRA},		// 4-55 (p159)	TODO: confirm that this really, really is just a special case of Bcc.
			{0xf000, 0x6000, Operation::Bcc, Decoder::Bcc_BSR},	// 4-25 (p129) and 4-59 (p163)

			{0xf1c0, 0x41c0, Operation::MOVEAl, Decoder::LEA},	// 4-110 (p214)
			{0xf100, 0x7000, Operation::MOVEq, Decoder::MOVEq},	// 4-134 (p238)

			{0xffff, 0x4e70, Operation::None, Decoder::RESET},	// 6-83 (p537)

			{0xffc0, 0x4ec0, Operation::JMP, Decoder::JMP},		// 4-108 (p212)
			{0xffc0, 0x4e80, Operation::JMP, Decoder::JSR},		// 4-109 (p213)
			{0xffff, 0x4e75, Operation::JMP, Decoder::RTS},		// 4-169 (p273)

			{0xf0c0, 0x9000, Operation::SUBb, Decoder::ADD_SUB},	// 4-174 (p278)
			{0xf0c0, 0x9040, Operation::SUBw, Decoder::ADD_SUB},	// 4-174 (p278)
			{0xf0c0, 0x9080, Operation::SUBl, Decoder::ADD_SUB},	// 4-174 (p278)

			{0xf0c0, 0xd000, Operation::ADDb, Decoder::ADD_SUB},	// 4-4 (p108)
			{0xf0c0, 0xd040, Operation::ADDw, Decoder::ADD_SUB},	// 4-4 (p108)
			{0xf0c0, 0xd080, Operation::ADDl, Decoder::ADD_SUB},	// 4-4 (p108)

			{0xf1c0, 0xd0c0, Operation::ADDAw, Decoder::ADDA_SUBA},	// 4-7 (p111)
			{0xf1c0, 0xd1c0, Operation::ADDAl, Decoder::ADDA_SUBA},	// 4-7 (p111)
			{0xf1c0, 0x90c0, Operation::SUBAw, Decoder::ADDA_SUBA},	// 4-177 (p281)
			{0xf1c0, 0x91c0, Operation::SUBAl, Decoder::ADDA_SUBA},	// 4-177 (p281)

			{0xf1c0, 0x5000, Operation::ADDQb, Decoder::ADDQ_SUBQ},	// 4-11 (p115)
			{0xf1c0, 0x5040, Operation::ADDQw, Decoder::ADDQ_SUBQ},	// 4-11 (p115)
			{0xf1c0, 0x5080, Operation::ADDQl, Decoder::ADDQ_SUBQ},	// 4-11 (p115)

			{0xf1c0, 0x5100, Operation::SUBQb, Decoder::ADDQ_SUBQ},	// 4-181 (p285)
			{0xf1c0, 0x5140, Operation::SUBQw, Decoder::ADDQ_SUBQ},	// 4-181 (p285)
			{0xf1c0, 0x5180, Operation::SUBQl, Decoder::ADDQ_SUBQ},	// 4-181 (p285)

			{0xf1c0, 0x0100, Operation::BTSTb, Decoder::BTST},		// 4-62 (p166)
			{0xffc0, 0x0800, Operation::BTSTb, Decoder::BTSTIMM},	// 4-63 (p167)

			{0xf1c0, 0x0180, Operation::BCLRb, Decoder::BCLR},		// 4-31 (p135)
			{0xffc0, 0x0880, Operation::BCLRb, Decoder::BCLRIMM},	// 4-32 (p136)

			{0xf0c0, 0x50c0, Operation::Scc, Decoder::Scc_DBcc},			// Scc: 4-173 (p276); DBcc: 4-91 (p195)

			{0xffc0, 0x4200, Operation::CLRb, Decoder::CLR_NEG_NEGX_NOT},	// 4-73 (p177)
			{0xffc0, 0x4240, Operation::CLRw, Decoder::CLR_NEG_NEGX_NOT},	// 4-73 (p177)
			{0xffc0, 0x4280, Operation::CLRl, Decoder::CLR_NEG_NEGX_NOT},	// 4-73 (p177)
			{0xffc0, 0x4400, Operation::NEGb, Decoder::CLR_NEG_NEGX_NOT},	// 4-144 (p248)
			{0xffc0, 0x4440, Operation::NEGw, Decoder::CLR_NEG_NEGX_NOT},	// 4-144 (p248)
			{0xffc0, 0x4480, Operation::NEGl, Decoder::CLR_NEG_NEGX_NOT},	// 4-144 (p248)
			{0xffc0, 0x4000, Operation::NEGXb, Decoder::CLR_NEG_NEGX_NOT},	// 4-146 (p250)
			{0xffc0, 0x4040, Operation::NEGXw, Decoder::CLR_NEG_NEGX_NOT},	// 4-146 (p250)
			{0xffc0, 0x4080, Operation::NEGXl, Decoder::CLR_NEG_NEGX_NOT},	// 4-146 (p250)
			{0xffc0, 0x4600, Operation::NOTb, Decoder::CLR_NEG_NEGX_NOT},	// 4-148 (p250)
			{0xffc0, 0x4640, Operation::NOTw, Decoder::CLR_NEG_NEGX_NOT},	// 4-148 (p250)
			{0xffc0, 0x4680, Operation::NOTl, Decoder::CLR_NEG_NEGX_NOT},	// 4-148 (p250)

			{0xf1d8, 0xe100, Operation::ASLb, Decoder::ASLR_LSLR_ROLR_ROXLRr},	// 4-22 (p126)
			{0xf1d8, 0xe140, Operation::ASLw, Decoder::ASLR_LSLR_ROLR_ROXLRr},	// 4-22 (p126)
			{0xf1d8, 0xe180, Operation::ASLl, Decoder::ASLR_LSLR_ROLR_ROXLRr},	// 4-22 (p126)
			{0xffc0, 0xe1c0, Operation::ASLm, Decoder::ASLR_LSLR_ROLR_ROXLRm},	// 4-22 (p126)

			{0xf1d8, 0xe000, Operation::ASRb, Decoder::ASLR_LSLR_ROLR_ROXLRr},	// 4-22 (p126)
			{0xf1d8, 0xe040, Operation::ASRw, Decoder::ASLR_LSLR_ROLR_ROXLRr},	// 4-22 (p126)
			{0xf1d8, 0xe080, Operation::ASRl, Decoder::ASLR_LSLR_ROLR_ROXLRr},	// 4-22 (p126)
			{0xffc0, 0xe0c0, Operation::ASRm, Decoder::ASLR_LSLR_ROLR_ROXLRm},	// 4-22 (p126)

			{0xf1d8, 0xe108, Operation::LSLb, Decoder::ASLR_LSLR_ROLR_ROXLRr},	// 4-113 (p217)
			{0xf1d8, 0xe148, Operation::LSLw, Decoder::ASLR_LSLR_ROLR_ROXLRr},	// 4-113 (p217)
			{0xf1d8, 0xe188, Operation::LSLl, Decoder::ASLR_LSLR_ROLR_ROXLRr},	// 4-113 (p217)
			{0xffc0, 0xe3c0, Operation::LSLm, Decoder::ASLR_LSLR_ROLR_ROXLRm},	// 4-113 (p217)

			{0xf1d8, 0xe008, Operation::LSRb, Decoder::ASLR_LSLR_ROLR_ROXLRr},	// 4-113 (p217)
			{0xf1d8, 0xe048, Operation::LSRw, Decoder::ASLR_LSLR_ROLR_ROXLRr},	// 4-113 (p217)
			{0xf1d8, 0xe088, Operation::LSRl, Decoder::ASLR_LSLR_ROLR_ROXLRr},	// 4-113 (p217)
			{0xffc0, 0xe2c0, Operation::LSRm, Decoder::ASLR_LSLR_ROLR_ROXLRm},	// 4-113 (p217)

			{0xf1d8, 0xe118, Operation::ROLb, Decoder::ASLR_LSLR_ROLR_ROXLRr},	// 4-160 (p264)
			{0xf1d8, 0xe158, Operation::ROLw, Decoder::ASLR_LSLR_ROLR_ROXLRr},	// 4-160 (p264)
			{0xf1d8, 0xe198, Operation::ROLl, Decoder::ASLR_LSLR_ROLR_ROXLRr},	// 4-160 (p264)
			{0xffc0, 0xe7c0, Operation::ROLm, Decoder::ASLR_LSLR_ROLR_ROXLRm},	// 4-160 (p264)

			{0xf1d8, 0xe018, Operation::RORb, Decoder::ASLR_LSLR_ROLR_ROXLRr},	// 4-160 (p264)
			{0xf1d8, 0xe058, Operation::RORw, Decoder::ASLR_LSLR_ROLR_ROXLRr},	// 4-160 (p264)
			{0xf1d8, 0xe098, Operation::RORl, Decoder::ASLR_LSLR_ROLR_ROXLRr},	// 4-160 (p264)
			{0xffc0, 0xe6c0, Operation::RORm, Decoder::ASLR_LSLR_ROLR_ROXLRm},	// 4-160 (p264)

			{0xf1d8, 0xe110, Operation::ROXLb, Decoder::ASLR_LSLR_ROLR_ROXLRr},	// 4-163 (p267)
			{0xf1d8, 0xe150, Operation::ROXLw, Decoder::ASLR_LSLR_ROLR_ROXLRr},	// 4-163 (p267)
			{0xf1d8, 0xe190, Operation::ROXLl, Decoder::ASLR_LSLR_ROLR_ROXLRr},	// 4-163 (p267)
			{0xffc0, 0xe5c0, Operation::ROXLm, Decoder::ASLR_LSLR_ROLR_ROXLRm},	// 4-163 (p267)

			{0xf1d8, 0xe010, Operation::ROXRb, Decoder::ASLR_LSLR_ROLR_ROXLRr},	// 4-163 (p267)
			{0xf1d8, 0xe050, Operation::ROXRw, Decoder::ASLR_LSLR_ROLR_ROXLRr},	// 4-163 (p267)
			{0xf1d8, 0xe090, Operation::ROXRl, Decoder::ASLR_LSLR_ROLR_ROXLRr},	// 4-163 (p267)
			{0xffc0, 0xe4c0, Operation::ROXRm, Decoder::ASLR_LSLR_ROLR_ROXLRm},	// 4-163 (p267)

			{0xffc0, 0x48c0, Operation::MOVEMtoMl, Decoder::MOVEM},				// 4-128 (p232)
			{0xffc0, 0x4880, Operation::MOVEMtoMw, Decoder::MOVEM},				// 4-128 (p232)
			{0xffc0, 0x4cc0, Operation::MOVEMtoRl, Decoder::MOVEM},				// 4-128 (p232)
			{0xffc0, 0x4c80, Operation::MOVEMtoRw, Decoder::MOVEM},				// 4-128 (p232)

			{0xffc0, 0x4a00, Operation::TSTb, Decoder::TST},					// 4-192 (p296)
			{0xffc0, 0x4a40, Operation::TSTw, Decoder::TST},					// 4-192 (p296)
			{0xffc0, 0x4a80, Operation::TSTl, Decoder::TST},					// 4-192 (p296)

			{0xf1c0, 0xc0c0, Operation::MULU, Decoder::MULU_MULS},				// 4-139 (p243)
			{0xf1c0, 0xc1c0, Operation::MULS, Decoder::MULU_MULS},				// 4-136 (p240)

			{0xfff0, 0x4e60, Operation::MOVEAl, Decoder::MOVEUSP},				// 6-21 (p475)

			{0xfff0, 0x4e40, Operation::TRAP, Decoder::TRAP},					// 4-188 (p292)

			{0xffff, 0x4e77, Operation::RTE_RTR, Decoder::RTE_RTR},				// 4-168 (p272) [RTR]
			{0xffff, 0x4e73, Operation::RTE_RTR, Decoder::RTE_RTR},				// 6-84 (p538) [RTE]

			{0xffff, 0x4e71, Operation::None, Decoder::NOP},					// 8-13 (p469)

			{0xf1f8, 0xc140, Operation::EXG, Decoder::EXG},						// 4-105 (p209)
			{0xf1f8, 0xc148, Operation::EXG, Decoder::EXG},						// 4-105 (p209)
			{0xf1f8, 0xc188, Operation::EXG, Decoder::EXG},						// 4-105 (p209)

			{0xfff8, 0x4840, Operation::SWAP, Decoder::EXT_SWAP},				// 4-185 (p289)

			{0xffff, 0x027c, Operation::ANDItoSR, Decoder::EORI_ORI_ANDI_SR},
			{0xffff, 0x023c, Operation::ANDItoCCR, Decoder::EORI_ORI_ANDI_SR},
			{0xffff, 0x0a7c, Operation::EORItoSR, Decoder::EORI_ORI_ANDI_SR},
			{0xffff, 0x0a3c, Operation::EORItoCCR, Decoder::EORI_ORI_ANDI_SR},
			{0xffff, 0x007c, Operation::ORItoSR, Decoder::EORI_ORI_ANDI_SR},
			{0xffff, 0x003c, Operation::ORItoCCR, Decoder::EORI_ORI_ANDI_SR},

			{0xf1c0, 0x0140, Operation::BCHGb, Decoder::BCHG_BSET},		// 4-28 (p132)
			{0xffc0, 0x0840, Operation::BCHGb, Decoder::BCHG_BSET},		// 4-29 (p133)
			{0xf1c0, 0x01c0, Operation::BSETb, Decoder::BCHG_BSET},		// 4-57 (p161)
			{0xffc0, 0x08c0, Operation::BSETb, Decoder::BCHG_BSET},		// 4-58 (p162)

			{0xffc0, 0x4ac0, Operation::TAS, Decoder::TAS},				// 4-186 (p290)

			{0xfff8, 0x4880, Operation::EXTbtow, Decoder::EXT_SWAP},		// 4-106 (p210)
			{0xfff8, 0x48c0, Operation::EXTwtol, Decoder::EXT_SWAP},		// 4-106 (p210)
		};

		std::vector<size_t> micro_op_pointers(65536, std::numeric_limits<size_t>::max());

		// The arbitrary_base is used so that the offsets returned by assemble_program into
		// storage_.all_bus_steps_ can be retained and mapped into the final version of
		// storage_.all_bus_steps_ at the end.
		BusStep arbitrary_base;

#define op(...) 	storage_.all_micro_ops_.emplace_back(__VA_ARGS__)
#define seq(...)	&arbitrary_base + assemble_program(__VA_ARGS__)
#define ea(n)		&storage_.effective_address_[n].full
#define a(n)		&storage_.address_[n].full

#define bw(x)		(x)
#define bw2(x, y)	(((x) << 8) | (y))
#define l(x)		(0x10000 | (x))
#define l2(x, y)	(0x10000 | ((x) << 8) | (y))

		// Perform a linear search of the mappings above for this instruction.
		for(size_t instruction = 0; instruction < 65536; ++instruction)	{
#ifndef NDEBUG
			int hits = 0;
#endif
			for(const auto &mapping: mappings) {
				if((instruction & mapping.mask) == mapping.value) {
					auto operation = mapping.operation;
					const auto micro_op_start = storage_.all_micro_ops_.size();

					// The following fields are used commonly enough to be worth pulling out here.
					const int ea_register = instruction & 7;
					const int ea_mode = (instruction >> 3) & 7;
					const int data_register = (instruction >> 9) & 7;
					const int op_mode = (instruction >> 6)&7;
					const bool op_mode_high_bit = !!(op_mode&4);

					// These are almost always true; they're non-const so that they can be corrected
					// by the few deviations.
					bool is_byte_access = (op_mode&3) == 0;
					bool is_long_word_access = (op_mode&3) == 2;

#define dec(n) decrement_action(is_long_word_access, is_byte_access, n)
#define inc(n) increment_action(is_long_word_access, is_byte_access, n)

					switch(mapping.decoder) {
						case Decoder::TAS: {
							const int mode = combined_mode(ea_mode, ea_register);
							storage_.instructions[instruction].set_destination(storage_, ea_mode, ea_register);
							switch(mode) {
								default: continue;

								case Dn:		// TAS Dn
									op(Action::PerformOperation, seq("np"));
								break;

								case Ind:		// TAS (An)
								case PostInc:	// TAS (An)+
									op(Action::None, seq("nrd", { a(ea_register) }, false));
									op(Action::PerformOperation, seq("tas np", { a(ea_register) }, false));
									if(mode == PostInc) {
										op(byte_inc(ea_register) | MicroOp::DestinationMask);
									}
								break;

								case PreDec:	// TAS -(An)
									op(byte_dec(ea_register) | MicroOp::DestinationMask, seq("n nrd", { a(ea_register) }, false));
									op(Action::PerformOperation, seq("tas np", { a(ea_register) }, false));
								break;

								case XXXl:		// TAS (xxx).l
									op(Action::None, seq("np"));
								case XXXw:		// TAS (xxx).w
								case d16An:		// TAS (d16, An)
								case d8AnXn:	// TAS (d8, An, Xn)
									op(address_action_for_mode(mode) | MicroOp::DestinationMask, seq("np nrd", { ea(1) }, false));
									op(Action::PerformOperation, seq("tas np", { ea(1) }, false));
								break;
							}
						} break;

						case Decoder::BCHG_BSET: {
							const int mode = combined_mode(ea_mode, ea_register);

							// Operations on a register are .l; all others are the default .b.
							if(ea_mode == Dn) {
								operation = (operation == Operation::BSETb) ? Operation::BSETl : Operation::BCHGl;
							}

							storage_.instructions[instruction].set_destination(storage_, ea_mode, ea_register);

							if(instruction & 0x100) {
								// The bit is nominated by a register.
								const int source_register = (instruction >> 9)&7;
								storage_.instructions[instruction].set_source(storage_, Dn, source_register);
							} else {
								// The bit is nominated by a constant, that will be obtained right here.
								storage_.instructions[instruction].set_source(storage_, Imm, 0);
								op(int(Action::AssembleWordDataFromPrefetch) | MicroOp::SourceMask, seq("np"));
							}

							switch(mode) {
								default: continue;

								case Dn:		// [BCHG/BSET].l Dn, Dn
									// Execution length depends on the selected bit, so allow flexible time for that.
									op(Action::None, seq("np"));
									op(Action::PerformOperation, seq("r"));
								break;

								case Ind:		// [BCHG/BSET].b Dn, (An)
								case PostInc:	// [BCHG/BSET].b Dn, (An)+
									op(Action::None, seq("nrd np", { a(ea_register) }, false));
									op(Action::PerformOperation, seq("nw", { a(ea_register) }, false));
									if(mode == PostInc) {
										op(byte_inc(ea_register) | MicroOp::DestinationMask);
									}
								break;

								case PreDec:	// [BCHG/BSET].b Dn, -(An)
									op(byte_dec(ea_register) | MicroOp::DestinationMask, seq("n nrd np", { a(ea_register) }, false));
									op(Action::PerformOperation, seq("nw", { a(ea_register) }, false));
								break;

								case XXXl:		// [BCHG/BSET].b Dn, (xxx).l
									op(Action::None, seq("np"));
								case XXXw:		// [BCHG/BSET].b Dn, (xxx).w
								case d16An:		// [BCHG/BSET].b Dn, (d16, An)
								case d8AnXn:	// [BCHG/BSET].b Dn, (d8, An, Xn)
									op(address_action_for_mode(mode) | MicroOp::DestinationMask, seq(pseq("np nrd np", mode), { ea(1) }, false));
									op(Action::PerformOperation, seq("nw", { ea(1) }, false));
								break;
							}
						} break;

						case Decoder::EORI_ORI_ANDI_SR: {
							// The source used here is always the high word of the prefetch queue.
							storage_.instructions[instruction].requires_supervisor = !(instruction & 0x40);
							op(Action::None, seq("np nn nn"));
							op(Action::PerformOperation, seq("np np"));
						} break;

						case Decoder::EXT_SWAP: {
							storage_.instructions[instruction].set_destination(storage_, Dn, ea_register);
							op(Action::PerformOperation, seq("np"));
						} break;

						case Decoder::EXG: {
							switch((instruction >> 3)&31) {
								default: continue;

								case 0x08:
									storage_.instructions[instruction].set_source(storage_, Dn, data_register);
									storage_.instructions[instruction].set_destination(storage_, Dn, ea_register);
								break;

								case 0x09:
									storage_.instructions[instruction].set_source(storage_, An, data_register);
									storage_.instructions[instruction].set_destination(storage_, An, ea_register);
								break;

								case 0x11:
									storage_.instructions[instruction].set_source(storage_, Dn, data_register);
									storage_.instructions[instruction].set_destination(storage_, An, ea_register);
								break;
							}

							op(Action::PerformOperation, seq("np"));
						} break;

						case Decoder::NOP: {
							op(Action::None, seq("np"));
						} break;

						case Decoder::RTE_RTR: {
							storage_.instructions[instruction].requires_supervisor = instruction == 0x4e73;

							// TODO: something explicit to ensure the nR nr nr is exclusively linked.
							op(Action::PrepareRTE_RTR, seq("nR nr nr", { &storage_.precomputed_addresses_[0], &storage_.precomputed_addresses_[1], &storage_.precomputed_addresses_[2] } ));
							op(Action::PerformOperation, seq("np np"));
							op();
						} break;

						case Decoder::AND_OR_EOR: {
							const bool to_ea = op_mode_high_bit;
							const bool is_eor = (instruction >> 12) == 0xb;

							// Weed out illegal operation modes.
							if(op_mode == 7) continue;

							const int mode = combined_mode(ea_mode, ea_register);

							if(to_ea) {
								storage_.instructions[instruction].set_destination(storage_, ea_mode, ea_register);
								storage_.instructions[instruction].set_source(storage_, Dn, data_register);

								// Only EOR takes Dn as a destination effective address.
								if(!is_eor && mode == Dn) continue;

								switch(is_long_word_access ? l(mode) : bw(mode)) {
									default: continue;

									case bw(Dn):		// EOR.bw Dn, Dn
										op(Action::PerformOperation, seq("np"));
									break;

									case l(Dn):			// EOR.l Dn, Dn
										op(Action::PerformOperation, seq("np nn"));
									break;

									case bw(Ind):		// [AND/OR/EOR].bw Dn, (An)
									case bw(PostInc):	// [AND/OR/EOR].bw Dn, (An)+
										op(Action::None, seq("nr", { a(ea_register) }, !is_byte_access));
										op(Action::PerformOperation, seq("np nw", { a(ea_register) }, !is_byte_access));
										if(mode == PostInc) {
											op(inc(ea_register) | MicroOp::DestinationMask);
										}
									break;

									case bw(PreDec):	// [AND/OR/EOR].bw Dn, -(An)
										op(dec(ea_register) | MicroOp::SourceMask, seq("n nrd", { a(ea_register) }, !is_byte_access));
										op(Action::PerformOperation, seq("np nw", { a(ea_register) }, !is_byte_access));
									break;

									case l(PreDec):		// [AND/OR/EOR].l Dn, -(An)
										op(int(Action::Decrement4) | MicroOp::DestinationMask, seq("n"));
									case l(Ind):		// [AND/OR/EOR].l Dn, (An)
									case l(PostInc):	// [AND/OR/EOR].l Dn, (An)+
										op(int(Action::CopyToEffectiveAddress) | MicroOp::DestinationMask, seq("nRd+ nrd", { ea(1), ea(1) }));
										op(Action::PerformOperation, seq("np nw- nW", { ea(1), ea(1) }));
										if(mode == PostInc) {
											op(int(Action::Increment4) | MicroOp::DestinationMask);
										}
									break;

									case bw(d16An):		// [AND/OR/EOR].bw Dn, (d16, An)
									case bw(d8AnXn):	// [AND/OR/EOR].bw Dn, (d8, An, Xn)
										op(calc_action_for_mode(mode) | MicroOp::DestinationMask, seq(pseq("np nrd", mode), { ea(1) }, !is_byte_access));
										op(Action::PerformOperation, seq("np nw", { ea(1) }, !is_byte_access));
									break;

									case l(d16An):		// [AND/OR/EOR].l Dn, (d16, An)
									case l(d8AnXn):		// [AND/OR/EOR].l Dn, (d8, An, Xn)
										op(calc_action_for_mode(mode) | MicroOp::DestinationMask, seq(pseq("np nRd+ nrd", mode), { ea(1), ea(1) }));
										op(Action::PerformOperation, seq("np nw- nW", { ea(1), ea(1) }));
									break;

									case bw(XXXl):		// [AND/OR/EOR].bw Dn, (xxx).l
										op(Action::None, seq("np"));
									case bw(XXXw):		// [AND/OR/EOR].bw Dn, (xxx).w
										op(address_assemble_for_mode(mode) | MicroOp::DestinationMask, seq("np nrd", { ea(1) }, !is_byte_access));
										op(Action::PerformOperation, seq("np nw", { ea(1) }, !is_byte_access));
									break;

									case l(XXXl):		// [AND/OR/EOR].l Dn, (xxx).l
										op(Action::None, seq("np"));
									case l(XXXw):		// [AND/OR/EOR].l Dn, (xxx).w
										op(address_assemble_for_mode(mode) | MicroOp::DestinationMask, seq("np nRd+ nrd", { ea(1), ea(1) }));
										op(Action::PerformOperation, seq("np nw- nW", { ea(1), ea(1) }));
									break;
								}
							} else {
								// EORs can be to EA only.
								if(is_eor) continue;

								storage_.instructions[instruction].set_source(storage_, ea_mode, ea_register);
								storage_.instructions[instruction].set_destination(storage_, Dn, data_register);

								switch(is_long_word_access ? l(mode) : bw(mode)) {
									default: continue;

									case bw(Dn):		// [AND/OR].bw Dn, Dn
										op(Action::PerformOperation, seq("np"));
									break;

									case l(Dn):			// [AND/OR].l Dn, Dn
										op(Action::PerformOperation, seq("np nn"));
									break;

									case bw(Ind):		// [AND/OR].bw (An), Dn
									case bw(PostInc):	// [AND/OR].bw (An)+, Dn
										op(Action::None, seq("nr", { a(ea_register) }, !is_byte_access));
										op(Action::PerformOperation, seq("np"));
										if(mode == PostInc) {
											op(inc(ea_register) | MicroOp::SourceMask);
										}
									break;

									case bw(PreDec):	// [AND/OR].bw -(An), Dn
										op(dec(ea_register) | MicroOp::SourceMask, seq("n nr", { a(ea_register) }, !is_byte_access));
										op(Action::PerformOperation, seq("np"));
									break;

									case l(PreDec):		// [AND/OR].l -(An), Dn
										op(int(Action::Decrement4) | MicroOp::SourceMask, seq("n"));
									case l(Ind):		// [AND/OR].l (An), Dn,
									case l(PostInc):	// [AND/OR].l (An)+, Dn
										op(int(Action::CopyToEffectiveAddress) | MicroOp::SourceMask, seq("nR+ nr", { ea(0), ea(0) }));
										op(Action::PerformOperation, seq("np n"));
										if(mode == PostInc) {
											op(int(Action::Increment4) | MicroOp::SourceMask);
										}
									break;

									case bw(d16An):		// [AND/OR].bw (d16, An), Dn
									case bw(d16PC):		// [AND/OR].bw (d16, PC), Dn
									case bw(d8AnXn):	// [AND/OR].bw (d8, An, Xn), Dn
									case bw(d8PCXn):	// [AND/OR].bw (d8, PX, Xn), Dn
										op(calc_action_for_mode(mode) | MicroOp::SourceMask, seq(pseq("np nr", mode), { ea(0) }, !is_byte_access));
										op(Action::PerformOperation, seq("np"));
									break;

									case l(d16An):		// [AND/OR].l (d16, An), Dn
									case l(d16PC):		// [AND/OR].l (d16, PC), Dn
									case l(d8AnXn):		// [AND/OR].l (d8, An, Xn), Dn
									case l(d8PCXn):		// [AND/OR].l (d8, PX, Xn), Dn
										op(calc_action_for_mode(mode) | MicroOp::SourceMask, seq(pseq("np nR+ nr", mode), { ea(0), ea(0) }));
										op(Action::PerformOperation, seq("np n"));
									break;

									case bw(XXXl):		// [AND/OR].bw (xxx).l, Dn
										op(Action::None, seq("np"));
									case bw(XXXw):		// [AND/OR].bw (xxx).w, Dn
										op(address_assemble_for_mode(mode) | MicroOp::SourceMask, seq("np nr", { ea(0) }, !is_byte_access));
										op(Action::PerformOperation, seq("np"));
									break;

									case l(XXXl):		// [AND/OR].bw (xxx).l, Dn
										op(Action::None, seq("np"));
									case l(XXXw):		// [AND/OR].bw (xxx).w, Dn
										op(address_assemble_for_mode(mode) | MicroOp::SourceMask, seq("np nR+ nr", { ea(0), ea(0) }));
										op(Action::PerformOperation, seq("np n"));
									break;

									case bw(Imm):		// [AND/OR].bw #, Dn
										op(int(Action::AssembleWordDataFromPrefetch) | MicroOp::SourceMask, seq("np"));
										op(Action::PerformOperation, seq("np"));
									break;

									case l(Imm):		// [AND/OR].l #, Dn
										op(Action::None, seq("np"));
										op(int(Action::AssembleLongWordDataFromPrefetch) | MicroOp::SourceMask, seq("np"));
										op(Action::PerformOperation, seq("np nn"));
									break;
								}
							}
						} break;

						case Decoder::MULU_MULS: {
							const int destination_register = (instruction >> 9) & 7;
							storage_.instructions[instruction].set_source(storage_, ea_mode, ea_register);
							storage_.instructions[instruction].set_destination(storage_, Dn, destination_register);

							const int mode = combined_mode(ea_mode, ea_register);
							switch(mode) {
								default: continue;

								case Dn:
									op(Action::None, seq("np"));
									op(Action::PerformOperation, seq("r"));
								break;

								case Ind:
								case PostInc:
									op(Action::None, seq("nr np", { a(ea_register) }));
									op(Action::PerformOperation, seq("r"));
									if(mode == PostInc) {
										op(int(Action::Increment2) | MicroOp::SourceMask);
									}
								break;

								case PreDec:
									op(int(Action::Decrement2) | MicroOp::SourceMask, seq("n nr np", { a(ea_register) }));
									op(Action::PerformOperation, seq("r"));
								break;

								case d16An:
								case d16PC:
								case d8AnXn:
								case d8PCXn:
									op(calc_action_for_mode(mode) | MicroOp::SourceMask, seq(pseq("n np nr np", mode), { ea(0) }));
									op(Action::PerformOperation, seq("r"));
								break;

								case XXXl:
									op(Action::None, seq("np"));
								case XXXw:
									op(address_assemble_for_mode(mode) | MicroOp::SourceMask, seq("np nr np", { ea(0) }));
									op(Action::PerformOperation, seq("r"));
								break;

								case Imm:
									// DEVIATION FROM YACHT.TXT. It has an additional np, which I need to figure out.
									op(int(Action::AssembleWordDataFromPrefetch) | MicroOp::SourceMask, seq("np np"));
									op(Action::PerformOperation, seq("r"));
								break;
							}
						} break;

						case Decoder::EORI_ORI_ANDI_SUBI_ADDI: {
							const int mode = combined_mode(ea_mode, ea_register);

							// Source is always something cribbed from the instruction stream;
							// destination is going to be in the write address unit.
							storage_.instructions[instruction].source = &storage_.source_bus_data_[0];
							if(mode == Dn) {
								storage_.instructions[instruction].destination = &storage_.data_[ea_register];
							} else {
								storage_.instructions[instruction].destination = &storage_.destination_bus_data_[0];
								storage_.instructions[instruction].destination_address = &storage_.address_[ea_register];
							}

							switch(is_long_word_access ? l(mode) : bw(mode)) {
								default: continue;

								case bw(Dn):			// [EORI/ORI/ANDI/SUBI/ADDI].bw #, Dn
									op(int(Action::AssembleWordDataFromPrefetch) | MicroOp::SourceMask, seq("np np"));
									op(Action::PerformOperation);
								break;

								case l(Dn):				// [EORI/ORI/ANDI/SUBI/ADDI].l #, Dn
									op(Action::None, seq("np"));
									op(int(Action::AssembleLongWordDataFromPrefetch) | MicroOp::SourceMask, seq("np np nn"));
									op(Action::PerformOperation);
								break;

								case bw(Ind):		// [EORI/ORI/ANDI/SUBI/ADDI].bw #, (An)
								case bw(PostInc):	// [EORI/ORI/ANDI/SUBI/ADDI].bw #, (An)+
									op(	int(Action::AssembleWordDataFromPrefetch) | MicroOp::SourceMask,
										seq("np nrd np", { a(ea_register) }, !is_byte_access));
									op(Action::PerformOperation, seq("nw", { a(ea_register) }, !is_byte_access));
									if(mode == PostInc) {
										op(inc(ea_register) | MicroOp::DestinationMask);
									}
								break;

								case l(Ind):		// [EORI/ORI/ANDI/SUBI/ADDI].l #, (An)
								case l(PostInc):	// [EORI/ORI/ANDI/SUBI/ADDI].l #, (An)+
									op(int(Action::CopyToEffectiveAddress) | MicroOp::DestinationMask, seq("np"));
									op(	int(Action::AssembleLongWordDataFromPrefetch) | MicroOp::SourceMask,
										seq("np nRd+ nrd np", { ea(1), ea(1) }));
									op(Action::PerformOperation, seq("nw- nW", { ea(1), ea(1) }));
									if(mode == PostInc) {
										op(int(Action::Increment4) | MicroOp::DestinationMask);
									}
								break;

								case bw(PreDec):	// [EORI/ORI/ANDI/SUBI/ADDI].bw #, -(An)
									op(dec(ea_register) | MicroOp::DestinationMask);
									op(	int(Action::AssembleWordDataFromPrefetch) | MicroOp::SourceMask,
										seq("np n nrd np", { a(ea_register) }, !is_byte_access));
									op(Action::PerformOperation, seq("nw", { a(ea_register) }, !is_byte_access));
								break;

								case l(PreDec):		// [EORI/ORI/ANDI/SUBI/ADDI].l #, -(An)
									op(int(Action::Decrement4) | MicroOp::DestinationMask);
									op(int(Action::CopyToEffectiveAddress) | MicroOp::DestinationMask, seq("np"));
									op(	int(Action::AssembleLongWordDataFromPrefetch) | MicroOp::SourceMask,
										seq("np n nRd+ nrd np", { ea(1), ea(1) }));
									op(Action::PerformOperation, seq("nw- nW", { ea(1), ea(1) }));
								break;

								case bw(d8AnXn):	// [EORI/ORI/ANDI/SUBI/ADDI].bw #, (d8, An, Xn)
								case bw(d16An):		// [EORI/ORI/ANDI/SUBI/ADDI].bw #, (d16, An)
									op(int(Action::AssembleWordDataFromPrefetch) | MicroOp::SourceMask, seq("np"));
									op(	calc_action_for_mode(mode) | MicroOp::DestinationMask,
										seq(pseq("np nrd np", mode), { ea(1) }, !is_byte_access));
									op(Action::PerformOperation, seq("nw", { ea(1) }, !is_byte_access));
								break;

								case l(d8AnXn):		// [EORI/ORI/ANDI/SUBI/ADDI].l #, (d8, An, Xn)
								case l(d16An):		// [EORI/ORI/ANDI/SUBI/ADDI].l #, (d16, An)
									op(Action::None, seq("np"));
									op(int(Action::AssembleLongWordDataFromPrefetch) | MicroOp::SourceMask, seq("np"));
									op(	calc_action_for_mode(mode) | MicroOp::DestinationMask,
										seq(pseq("np nRd+ nrd np", mode), { ea(1), ea(1) }));
									op(Action::PerformOperation, seq("nw- nW", { ea(1), ea(1) }));
								break;

								case bw(XXXw):		// [EORI/ORI/ANDI/SUBI/ADDI].bw #, (xxx).w
									op(int(Action::AssembleWordDataFromPrefetch) | MicroOp::SourceMask, seq("np"));
									op(	int(Action::AssembleWordAddressFromPrefetch) | MicroOp::DestinationMask,
										seq("np nrd np", { ea(1) }, !is_byte_access));
									op(Action::PerformOperation, seq("nw", { ea(1) }, !is_byte_access));
								break;

								case l(XXXw):		// [EORI/ORI/ANDI/SUBI/ADDI].l #, (xxx).w
									op(Action::None, seq("np"));
									op(int(Action::AssembleLongWordDataFromPrefetch) | MicroOp::SourceMask, seq("np"));
									op(	int(Action::AssembleWordAddressFromPrefetch) | MicroOp::DestinationMask,
										seq("np nRd+ nrd np", { ea(1), ea(1) }));
									op(Action::PerformOperation, seq("nw- nW", { ea(1), ea(1) }));
								break;

								case bw(XXXl):		// [EORI/ORI/ANDI/SUBI/ADDI].bw #, (xxx).l
									op(int(Action::AssembleWordDataFromPrefetch) | MicroOp::SourceMask, seq("np np"));
									op(	int(Action::AssembleLongWordAddressFromPrefetch) | MicroOp::DestinationMask,
										seq("np nrd np", { ea(1) }, !is_byte_access));
									op(Action::PerformOperation, seq("nw", { ea(1) }, !is_byte_access));
								break;

								case l(XXXl):		// [EORI/ORI/ANDI/SUBI/ADDI].l #, (xxx).l
									op(Action::None, seq("np"));
									op(int(Action::AssembleLongWordDataFromPrefetch) | MicroOp::SourceMask, seq("np np"));
									op(	int(Action::AssembleLongWordAddressFromPrefetch) | MicroOp::DestinationMask,
										seq("np nRd+ nrd np", { ea(1), ea(1) }));
									op(Action::PerformOperation, seq("nw- nW", { ea(1), ea(1) }));
								break;
							}
						} break;

						case Decoder::ADD_SUB: {
							// ADD and SUB definitely always involve a data register and an arbitrary addressing mode;
							// which direction they operate in depends on bit 8.
							const bool reverse_source_destination = !(instruction & 256);

							const int mode = combined_mode(ea_mode, ea_register);

							if(reverse_source_destination) {
								storage_.instructions[instruction].destination = &storage_.data_[data_register];
								storage_.instructions[instruction].source = &storage_.source_bus_data_[0];
								storage_.instructions[instruction].source_address = &storage_.address_[ea_register];

								// Perform [ADD/SUB].blw <ea>, Dn
								switch(is_long_word_access ? l(mode) : bw(mode)) {
									default: continue;

									case bw(Dn):		// ADD/SUB.bw Dn, Dn
										storage_.instructions[instruction].source = &storage_.data_[ea_register];
										op(Action::PerformOperation, seq("np"));
									break;

									case l(Dn): 		// ADD/SUB.l Dn, Dn
										storage_.instructions[instruction].source = &storage_.data_[ea_register];
										op(Action::PerformOperation, seq("np nn"));
									break;

									case bw(An):		// ADD/SUB.bw An, Dn
										// Address registers can't provide single bytes.
										if(is_byte_access) continue;
										storage_.instructions[instruction].source = &storage_.address_[ea_register];
										op(Action::PerformOperation, seq("np"));
									break;

									case l(An):			// ADD/SUB.l An, Dn
										storage_.instructions[instruction].source = &storage_.address_[ea_register];
										op(Action::PerformOperation, seq("np nn"));
									break;

									case bw(Ind):		// ADD/SUB.bw (An), Dn
									case bw(PostInc):	// ADD/SUB.bw (An)+, Dn
										op(Action::None, seq("nr np", { a(ea_register) }, !is_byte_access));
										if(ea_mode == PostInc) {
											op(inc(ea_register) | MicroOp::SourceMask);
										}
										op(Action::PerformOperation);
									break;

									case l(Ind):		// ADD/SUB.l (An), Dn
									case l(PostInc):	// ADD/SUB.l (An)+, Dn
										op(	int(Action::CopyToEffectiveAddress) | MicroOp::SourceMask,
											seq("nR+ nr np n", { ea(0), ea(0) }));
										if(mode == PostInc) {
											op(int(Action::Increment4) | MicroOp::SourceMask);
										}
										op(Action::PerformOperation);
									break;

									case bw(PreDec):	// ADD/SUB.bw -(An), Dn
										op(	dec(ea_register) | MicroOp::SourceMask,
											seq("n nr np", { a(ea_register) }, !is_byte_access));
										op(Action::PerformOperation);
									break;

									case l(PreDec):		// ADD/SUB.l -(An), Dn
										op(int(Action::Decrement4) | MicroOp::SourceMask);
										op(	int(Action::CopyToEffectiveAddress) | MicroOp::SourceMask,
											seq("n nR+ nr np n", { ea(0), ea(0) }));
										op(Action::PerformOperation);
									break;

									case bw(XXXl):		// ADD/SUB.bw (xxx).l, Dn
										op(Action::None, seq("np"));
									case bw(XXXw):		// ADD/SUB.bw (xxx).w, Dn
										op(	address_assemble_for_mode(mode) | MicroOp::SourceMask,
											seq("np nr np", { ea(0) }, !is_byte_access));
										op(Action::PerformOperation);
									break;

									case l(XXXl):		// ADD/SUB.l (xxx).l, Dn
										op(Action::None, seq("np"));
									case l(XXXw):		// ADD/SUB.l (xxx).w, Dn
										op(	address_assemble_for_mode(mode) | MicroOp::SourceMask,
											seq("np nR+ nr np n", { ea(0), ea(0) }));
										op(Action::PerformOperation);
									break;

									case bw(d16PC):		// ADD/SUB.bw (d16, PC), Dn
									case bw(d8PCXn):	// ADD/SUB.bw (d8, PC, Xn), Dn
									case bw(d16An):		// ADD/SUB.bw (d16, An), Dn
									case bw(d8AnXn):	// ADD/SUB.bw (d8, An, Xn), Dn
										op(	calc_action_for_mode(mode) | MicroOp::SourceMask,
											seq(pseq("np nr np", mode), { ea(0) }, !is_byte_access));
										op(Action::PerformOperation);
									break;

									case l(d16PC):		// ADD/SUB.l (d16, PC), Dn
									case l(d8PCXn):		// ADD/SUB.l (d8, PC, Xn), Dn
									case l(d16An):		// ADD/SUB.l (d16, An), Dn
									case l(d8AnXn):		// ADD/SUB.l (d8, An, Xn), Dn
										op(	calc_action_for_mode(mode) | MicroOp::SourceMask,
											seq(pseq("np nR+ nr np n", mode), { ea(0), ea(0) }));
										op(Action::PerformOperation);
									break;

									case bw(Imm):		// ADD/SUB.bw #, Dn
										op(int(Action::AssembleWordDataFromPrefetch) | MicroOp::SourceMask, seq("np np"));
										op(Action::PerformOperation);
									break;

									case l(Imm):		// ADD/SUB.l #, Dn
										op(Action::None, seq("np"));
										op(int(Action::AssembleLongWordDataFromPrefetch) | MicroOp::SourceMask, seq("np np nn"));
										op(Action::PerformOperation);
									break;
								}
							} else {
								storage_.instructions[instruction].source = &storage_.data_[data_register];

								const auto destination_register = ea_register;
								storage_.instructions[instruction].destination = &storage_.destination_bus_data_[0];
								storage_.instructions[instruction].destination_address = &storage_.address_[destination_register];

								// Perform [ADD/SUB].blw Dn, <ea>
								switch(is_long_word_access ? l(mode) : bw(mode)) {
									default: continue;

									case bw(Ind):		// ADD/SUB.bw Dn, (An)
									case bw(PostInc):	// ADD/SUB.bw Dn, (An)+
										op(Action::None, seq("nrd np", { a(destination_register) }, !is_byte_access));
										op(Action::PerformOperation, seq("nw", { a(destination_register) }, !is_byte_access));
										if(ea_mode == PostInc) {
											op(inc(destination_register) | MicroOp::DestinationMask);
										}
									break;

									case l(Ind):		// ADD/SUB.l Dn, (An)
									case l(PostInc):	// ADD/SUB.l Dn, (An)+
										op(int(Action::CopyToEffectiveAddress) | MicroOp::DestinationMask, seq("nRd+ nrd np", { ea(1), ea(1) }));
										op(Action::PerformOperation, seq("nw- nW", { ea(1), ea(1) }));
										if(ea_mode == PostInc) {
											op(int(Action::Increment4) | MicroOp::DestinationMask);
										}
									break;

									case bw(PreDec):	// ADD/SUB.bw Dn, -(An)
										op(	dec(destination_register) | MicroOp::DestinationMask,
											seq("n nrd np", { a(destination_register) }, !is_byte_access));
										op(Action::PerformOperation, seq("nw", { a(destination_register) }, !is_byte_access));
									break;

									case l(PreDec):		// ADD/SUB.l Dn, -(An)
										op(	int(Action::Decrement4) | MicroOp::DestinationMask);
										op(	int(Action::CopyToEffectiveAddress) | MicroOp::DestinationMask,
											seq("n nRd+ nrd np", { ea(1), ea(1) }));
										op(	Action::PerformOperation,
											seq("nw- nW", { ea(1), ea(1) }));
									break;

									case bw(d16An):		// ADD/SUB.bw (d16, An), Dn
									case bw(d8AnXn):	// ADD/SUB.bw (d8, An, Xn), Dn
										op(	calc_action_for_mode(mode) | MicroOp::DestinationMask,
											seq(pseq("np nrd np", mode), { ea(1) }, !is_byte_access));
										op(Action::PerformOperation, seq("nw", { ea(1) }, !is_byte_access));
									break;

									case l(d16An):		// ADD/SUB.l (d16, An), Dn
									case l(d8AnXn):		// ADD/SUB.l (d8, An, Xn), Dn
										op(	calc_action_for_mode(mode) | MicroOp::DestinationMask,
											seq(pseq("np nRd+ nrd np", mode), { ea(1), ea(1) }));
										op(Action::PerformOperation, seq("nw- nW", { ea(1), ea(1) }));
									break;

									case bw(XXXl):		// ADD/SUB.bw Dn, (xxx).l
										op(Action::None, seq("np"));
									case bw(XXXw):		// ADD/SUB.bw Dn, (xxx).w
										op(	address_assemble_for_mode(mode) | MicroOp::DestinationMask,
											seq("np nrd np", { ea(1) }, !is_byte_access));
										op(Action::PerformOperation, seq("nw", { ea(1) }, !is_byte_access));
									break;

									case l(XXXl):		// ADD/SUB.l Dn, (xxx).l
										op(Action::None, seq("np"));
									case l(XXXw):		// ADD/SUB.l Dn, (xxx).w
										op(	address_assemble_for_mode(mode) | MicroOp::DestinationMask,
											seq("np nRd+ nrd np", { ea(1), ea(1) }));
										op(	Action::PerformOperation,
											seq("nw- nW", { ea(1), ea(1) }));
									break;
								}
							}
						} break;

						case Decoder::ADDA_SUBA: {
							const int destination_register = (instruction >> 9) & 7;
							storage_.instructions[instruction].set_destination(storage_, 1, destination_register);
							storage_.instructions[instruction].set_source(storage_, ea_mode, ea_register);

							const int mode = combined_mode(ea_mode, ea_register);
							is_long_word_access = op_mode_high_bit;

							switch(is_long_word_access ? l(mode) : bw(mode)) {
								default: continue;

								case bw(Dn):		// ADDA/SUBA.w Dn, An
								case bw(An):		// ADDA/SUBA.w An, An
								case l(Dn):			// ADDA/SUBA.l Dn, An
								case l(An):			// ADDA/SUBA.l An, An
									op(Action::PerformOperation, seq("np nn"));
								break;

								case bw(Ind):		// ADDA/SUBA.w (An), An
								case bw(PostInc):	// ADDA/SUBA.w (An)+, An
									op(Action::None, seq("nr np nn", { a(ea_register) }));
									if(ea_mode == PostInc) {
										op(int(Action::Increment2) | MicroOp::SourceMask);
									}
									op(Action::PerformOperation);
								break;

								case l(Ind):		// ADDA/SUBA.l (An), An
								case l(PostInc):	// ADDA/SUBA.l (An)+, An
									op(int(Action::CopyToEffectiveAddress) | MicroOp::SourceMask, seq("nR+ nr np n", { ea(0), ea(0) }));
									if(mode == PostInc) {
										op(int(Action::Increment4) | MicroOp::SourceMask);
									}
									op(Action::PerformOperation);
								break;

								case bw(PreDec):	// ADDA/SUBA.w -(An), An
									op(int(Action::Decrement2) | MicroOp::SourceMask);
									op(Action::None, seq("n nr np nn", { a(ea_register) }));
									op(Action::PerformOperation);
								break;

								case l(PreDec):		// ADDA/SUBA.l -(An), An
									op(int(Action::Decrement4) | MicroOp::SourceMask);
									op(int(Action::CopyToEffectiveAddress) | MicroOp::SourceMask, seq("n nR+ nr np n", { ea(0), ea(0) }));
									op(Action::PerformOperation);
								break;

								case bw(d16An):		// ADDA/SUBA.w (d16, An), An
								case bw(d8AnXn):	// ADDA/SUBA.w (d8, An, Xn), An
									op(	calc_action_for_mode(mode) | MicroOp::SourceMask,
										seq(pseq("np nr np nn", mode), { ea(0) }));
									op(Action::PerformOperation);
								break;

								case l(d16An):		// ADDA/SUBA.l (d16, An), An
								case l(d8AnXn):		// ADDA/SUBA.l (d8, An, Xn), An
									op(	calc_action_for_mode(mode) | MicroOp::SourceMask,
										seq(pseq("np nR+ nr np n", mode), { ea(0), ea(0) }));
									op(Action::PerformOperation);
								break;

								case bw(XXXl):		// ADDA/SUBA.w (xxx).l, An
									op(Action::None, seq("np"));
								case bw(XXXw):		// ADDA/SUBA.w (xxx).w, An
									op(	address_assemble_for_mode(mode) | MicroOp::SourceMask,
										seq("np nr np nn", { ea(0) }));
									op(Action::PerformOperation);
								break;

								case l(XXXl):		// ADDA/SUBA.l (xxx).l, An
									op(Action::None, seq("np"));
								case l(XXXw):		// ADDA/SUBA.l (xxx).w, An
									op(	address_assemble_for_mode(mode) | MicroOp::SourceMask,
										seq("np nR+ nr np n", { ea(0), ea(0) }));
									op(	Action::PerformOperation);
								break;

								case bw(Imm):		// ADDA/SUBA.w #, An
									op(int(Action::AssembleWordDataFromPrefetch) | MicroOp::SourceMask, seq("np np nn"));
									op(Action::PerformOperation);
								break;

								case l(Imm):		// ADDA/SUBA.l #, Dn
									op(Action::None, seq("np"));
									op(int(Action::AssembleLongWordDataFromPrefetch) | MicroOp::SourceMask, seq("np np nn"));
									op(Action::PerformOperation);
								break;
							}
						} break;

						case Decoder::ADDQ_SUBQ: {
							storage_.instructions[instruction].set_destination(storage_, ea_mode, ea_register);

							const int mode = combined_mode(ea_mode, ea_register);

							// If the destination is an address register then byte mode isn't allowed, and
							// flags shouldn't be affected (so, a different operation is used).
							if(mode == An) {
								if(is_byte_access) continue;
								switch(operation) {
									default: break;
									case Operation::ADDQl:	// TODO: should the adds be distinguished? If so, how?
									case Operation::ADDQw:	operation = Operation::ADDQAl;	break;
									case Operation::SUBQl:
									case Operation::SUBQw:	operation = Operation::SUBQAl;	break;
								}
							}

							switch(is_long_word_access ? l(mode) : bw(mode)) {
								default: continue;

								case bw(Dn):		// [ADD/SUB]Q.bw #, Dn
									op(Action::PerformOperation, seq("np"));
								break;

								case l(Dn):			// [ADD/SUB]Q.l #, Dn
								case l(An):			// [ADD/SUB]Q.l #, An
								case bw(An):		// [ADD/SUB]Q.bw #, An
									op(Action::PerformOperation, seq("np nn"));
								break;

								case bw(Ind):		// [ADD/SUB]Q.bw #, (An)
								case bw(PostInc):	// [ADD/SUB]Q.bw #, (An)+
									op(Action::None, seq("nrd np", { a(ea_register) }, !is_byte_access));
									op(Action::PerformOperation, seq("nw", { a(ea_register) }, !is_byte_access));
									if(mode == PostInc) {
										op(inc(ea_register) | MicroOp::DestinationMask);
									}
								break;

								case l(PreDec):		// [ADD/SUB]Q.l #, -(An)
									op(int(Action::Decrement4) | MicroOp::DestinationMask, seq("n"));
								case l(Ind):		// [ADD/SUB]Q.l #, (An)
								case l(PostInc):	// [ADD/SUB]Q.l #, (An)+
									op(int(Action::CopyToEffectiveAddress) | MicroOp::DestinationMask, seq("nRd+ nrd np", { ea(1), ea(1) }));
									op(Action::PerformOperation, seq("nw- nW", { ea(1), ea(1) }));
									if(mode == PostInc) {
										op(int(Action::Increment4) | MicroOp::DestinationMask);
									}
								break;

								case bw(PreDec):	// [ADD/SUB]Q.bw #, -(An)
									op(	dec(ea_register) | MicroOp::DestinationMask,
										seq("n nrd np", { a(ea_register) }, !is_byte_access));
									op(Action::PerformOperation, seq("nw", { a(ea_register) }, !is_byte_access));
								break;

								case bw(d16An):		// [ADD/SUB]Q.bw #, (d16, An)
								case bw(d8AnXn):	// [ADD/SUB]Q.bw #, (d8, An, Xn)
									op(calc_action_for_mode(mode) | MicroOp::DestinationMask, seq(pseq("np nrd np", mode), { ea(1) }, !is_byte_access));
									op(Action::PerformOperation, seq("nw", { ea(1) }, !is_byte_access));
								break;

								case l(d16An):		// [ADD/SUB]Q.l #, (d16, An)
								case l(d8AnXn):		// [ADD/SUB]Q.l #, (d8, An, Xn)
									op(calc_action_for_mode(mode) | MicroOp::DestinationMask, seq(pseq("np nRd+ nrd np", mode), { ea(1), ea(1) }));
									op(Action::PerformOperation, seq("nw- nW", { ea(1), ea(1) }));
								break;

								case bw(XXXl):		// [ADD/SUB]Q.bw #, (xxx).l
									op(Action::None, seq("np"));
								case bw(XXXw):		// [ADD/SUB]Q.bw #, (xxx).w
									op(address_assemble_for_mode(mode) | MicroOp::DestinationMask, seq("np nrd np", { ea(1) }, !is_byte_access));
									op(Action::PerformOperation, seq("nw", { ea(1) }, !is_byte_access));
								break;

								case l(XXXl):		// [ADD/SUB]Q.l #, (xxx).l
									op(Action::None, seq("np"));
								case l(XXXw):		// [ADD/SUB]Q.l #, (xxx).w
									op(address_assemble_for_mode(mode) | MicroOp::DestinationMask, seq("np nRd+ nrd np", { ea(1), ea(1) }));
									op(Action::PerformOperation, seq("nw- nW", { ea(1), ea(1) }));
								break;
							}
						} break;

						// This decoder actually decodes nothing; it just schedules a PerformOperation followed by an empty step.
						case Decoder::Bcc_BSR: {
							const int condition = (instruction >> 8) & 0xf;
							if(condition == 1) {
								// This is BSR, which is unconditional and means pushing a return address to the stack first.

								// Push the return address to the stack.
								op(Action::PrepareBSR, seq("n nW+ nw", { ea(1), ea(1) }));
							}

							// This is Bcc.
							op(Action::PerformOperation);
							op();	// The above looks terminal, but will be dynamically reprogrammed.
						} break;

						// A little artificial, there's nothing really to decode for BRA.
						case Decoder::BRA: {
							op(Action::PerformOperation, seq("n np np"));
						} break;

						// Decodes a BTST, potential mutating the operation into a BTSTl,
						// or a BCLR.
						case Decoder::BCLR:
						case Decoder::BTST: {
							const bool is_bclr = mapping.decoder == Decoder::BCLR;

							const int mask_register = (instruction >> 9) & 7;
							storage_.instructions[instruction].set_source(storage_, 0, mask_register);
							storage_.instructions[instruction].set_destination(storage_, ea_mode, ea_register);

							const int mode = combined_mode(ea_mode, ea_register);
							switch(mode) {
								default: continue;

								case Dn:		// BTST.l Dn, Dn
									if(is_bclr) {
										operation = Operation::BCLRl;
										op(Action::None, seq("np"));
										op(Action::PerformOperation, seq("r"));
									} else {
										operation = Operation::BTSTl;
										op(Action::PerformOperation, seq("np n"));
									}
								break;

								case Ind:		// BTST.b Dn, (An)
								case PostInc:	// BTST.b Dn, (An)+
									op(Action::None, seq("nrd np", { a(ea_register) }, false));
									op(Action::PerformOperation, is_bclr ? seq("nw", { a(ea_register) }, false) : nullptr);
									if(mode == PostInc) {
										op(byte_inc(ea_register) | MicroOp::DestinationMask);
									}
								break;

								case PreDec:	// BTST.b Dn, -(An)
									op(byte_dec(ea_register) | MicroOp::DestinationMask, seq("n nrd np", { a(ea_register) }, false));
									op(Action::PerformOperation, is_bclr ? seq("nw", { a(ea_register) }, false) : nullptr);
								break;

								case d16An:		// BTST.b Dn, (d16, An)
								case d8AnXn:	// BTST.b Dn, (d8, An, Xn)
								case d16PC:		// BTST.b Dn, (d16, PC)
								case d8PCXn:	// BTST.b Dn, (d8, PC, Xn)
									// PC-relative addressing isn't support for BCLR.
									if((mode == d16PC || mode == d8PCXn) && is_bclr) continue;

									op(	calc_action_for_mode(mode) | MicroOp::DestinationMask,
										seq(pseq("np nrd np", mode), { ea(1) }, false));
									op(Action::PerformOperation, is_bclr ? seq("nw", { ea(1) }, false) : nullptr);
								break;

								case XXXl:	// BTST.b Dn, (xxx).l
									op(Action::None, seq("np"));
								case XXXw:	// BTST.b Dn, (xxx).w
									op(	address_assemble_for_mode(mode) | MicroOp::DestinationMask,
										seq("np nrd np", { ea(1) }, false));
									op(Action::PerformOperation, is_bclr ? seq("nw", { ea(1) }, false) : nullptr);
								break;
							}
						} break;

						case Decoder::BCLRIMM:
						case Decoder::BTSTIMM: {
							const bool is_bclr = mapping.decoder == Decoder::BCLRIMM;

							storage_.instructions[instruction].source = &storage_.source_bus_data_[0];
							storage_.instructions[instruction].set_destination(storage_, ea_mode, ea_register);

							const int mode = combined_mode(ea_mode, ea_register);
							switch(mode) {
								default: continue;

								case Dn:		// BTST.l #, Dn
									if(is_bclr) {
										operation = Operation::BCLRl;
										op(int(Action::AssembleWordDataFromPrefetch) | MicroOp::SourceMask, seq("np np"));
										op(Action::PerformOperation, seq("r"));
									} else {
										operation = Operation::BTSTl;
										op(int(Action::AssembleWordDataFromPrefetch) | MicroOp::SourceMask, seq("np np n"));
										op(Action::PerformOperation);
									}
								break;

								case Ind:		// BTST.b #, (An)
								case PostInc:	// BTST.b #, (An)+
									op(int(Action::AssembleWordDataFromPrefetch) | MicroOp::SourceMask, seq("np nrd np", { a(ea_register) }, false));
									op(Action::PerformOperation, is_bclr ? seq("nw", { a(ea_register) }, false) : nullptr);
									if(mode == PostInc) {
										op(byte_inc(ea_register) | MicroOp::DestinationMask);
									}
								break;

								case PreDec:	// BTST.b #, -(An)
									op(int(Action::AssembleWordDataFromPrefetch) | MicroOp::SourceMask, seq("np"));
									op(byte_dec(ea_register) | MicroOp::DestinationMask, seq("n nrd np", { a(ea_register) }, false));
									op(Action::PerformOperation, is_bclr ? seq("nw", { a(ea_register) }, false) : nullptr);
								break;

								case d16An:		// BTST.b #, (d16, An)
								case d8AnXn:	// BTST.b #, (d8, An, Xn)
								case d16PC:		// BTST.b #, (d16, PC)
								case d8PCXn:	// BTST.b #, (d8, PC, Xn)
									// PC-relative addressing isn't support for BCLR.
									if((mode == d16PC || mode == d8PCXn) && is_bclr) continue;

									op(int(Action::AssembleWordDataFromPrefetch) | MicroOp::SourceMask, seq("np"));
									op(	calc_action_for_mode(mode) | MicroOp::DestinationMask,
										seq(pseq("np nrd np", mode), { ea(1) }, false));
									op(Action::PerformOperation, is_bclr ? seq("nw", { ea(1) }, false) : nullptr);
								break;

								case XXXw:	// BTST.b #, (xxx).w
									op(	int(Action::AssembleWordDataFromPrefetch) | MicroOp::SourceMask, seq("np"));
									op(	int(Action::AssembleWordAddressFromPrefetch) | MicroOp::DestinationMask,
										seq("np nrd np", { ea(1) }, false));
									op(Action::PerformOperation, is_bclr ? seq("nw", { ea(1) }, false) : nullptr);
								break;

								case XXXl:	// BTST.b #, (xxx).l
									op(	int(Action::AssembleWordDataFromPrefetch) | MicroOp::SourceMask, seq("np np"));
									op(	int(Action::AssembleLongWordAddressFromPrefetch) | MicroOp::DestinationMask,
										seq("np nrd np", { ea(1) }, false));
									op(Action::PerformOperation, is_bclr ? seq("nw", { ea(1) }, false) : nullptr);
								break;
							}
						} break;

						// Decodes the format used by ABCD and SBCD.
						case Decoder::ABCD_SBCD: {
							const int destination_register = (instruction >> 9) & 7;

							if(instruction & 8) {
								storage_.instructions[instruction].source = &storage_.source_bus_data_[0];
								storage_.instructions[instruction].destination = &storage_.destination_bus_data_[0];
								storage_.instructions[instruction].source_address = &storage_.address_[ea_register];
								storage_.instructions[instruction].destination_address = &storage_.address_[destination_register];

								const int source_dec = dec(ea_register);
								const int destination_dec = dec(destination_register);

								int first_action = source_dec | MicroOp::SourceMask | MicroOp::DestinationMask;
								if(source_dec != destination_dec) {
									first_action = source_dec | MicroOp::SourceMask;
									op(destination_dec | MicroOp::DestinationMask);
								}

								op(	first_action,
									seq("n nr nr np nw", { a(ea_register), a(destination_register), a(destination_register) }, false));
								op(Action::PerformOperation);
							} else {
								storage_.instructions[instruction].source = &storage_.data_[ea_register];
								storage_.instructions[instruction].destination = &storage_.data_[destination_register];

								op(Action::PerformOperation, seq("np n"));
							}
						} break;

						case Decoder::ASLR_LSLR_ROLR_ROXLRr: {
							storage_.instructions[instruction].set_destination(storage_, 0, ea_register);

							// All further decoding occurs at runtime; that's also when the proper number of
							// no-op cycles will be scheduled.
							if(((instruction >> 6) & 3) == 2) {
								op(Action::None, seq("np nn"));
							} else {
								op(Action::None, seq("np n"));
							}

							// Use a no-op bus cycle that can have its length filled in later.
							op(Action::PerformOperation, seq("r"));
						} break;

						case Decoder::ASLR_LSLR_ROLR_ROXLRm: {
							storage_.instructions[instruction].set_destination(storage_, ea_mode, ea_register);

							const int mode = combined_mode(ea_mode, ea_register);
							switch(mode) {
								default: continue;

								case Ind:		// AS(L/R)/LS(L/R)/RO(L/R)/ROX(L/R).w (An)
								case PostInc:	// AS(L/R)/LS(L/R)/RO(L/R)/ROX(L/R).w (An)+
									op(Action::None, seq("nrd np", { a(ea_register) }));
									op(Action::PerformOperation, seq("nw", { a(ea_register) }));
									if(ea_mode == PostInc) {
										op(int(Action::Increment2) | MicroOp::DestinationMask);
									}
								break;

								case PreDec:	// AS(L/R)/LS(L/R)/RO(L/R)/ROX(L/R).w -(An)
									op(int(Action::Decrement2) | MicroOp::DestinationMask, seq("n nrd np", { a(ea_register) }));
									op(Action::PerformOperation, seq("nw", { a(ea_register) }));
								break;

								case d16An:		// AS(L/R)/LS(L/R)/RO(L/R)/ROX(L/R).w (d16, An)
								case d8AnXn:	// AS(L/R)/LS(L/R)/RO(L/R)/ROX(L/R).w (d8, An, Xn)
									op(	calc_action_for_mode(mode) | MicroOp::DestinationMask,
										seq(pseq("np nrd np", mode), { ea(1) }));
									op(Action::PerformOperation, seq("nw", { ea(1) }));
								break;

								case XXXl:	// AS(L/R)/LS(L/R)/RO(L/R)/ROX(L/R).w (xxx).l
									op(Action::None, seq("np"));
								case XXXw:	// AS(L/R)/LS(L/R)/RO(L/R)/ROX(L/R).w (xxx).w
									op(	address_assemble_for_mode(mode) | MicroOp::DestinationMask,
										seq("np nrd np", { ea(1) }));
									op(Action::PerformOperation, seq("nw", { ea(1) }));
								break;
							}
						} break;

						case Decoder::CLR_NEG_NEGX_NOT: {
							storage_.instructions[instruction].set_destination(storage_, ea_mode, ea_register);

							const int mode = combined_mode(ea_mode, ea_register);
							switch(is_long_word_access ? l(mode) : bw(mode)) {
								default: continue;

								case bw(Dn):		// [CLR/NEG/NEGX/NOT].bw Dn
								case l(Dn):			// [CLR/NEG/NEGX/NOT].l Dn
									op(Action::PerformOperation, seq("np"));
								break;

								case bw(Ind):		// [CLR/NEG/NEGX/NOT].bw (An)
								case bw(PostInc):	// [CLR/NEG/NEGX/NOT].bw (An)+
									op(Action::None, seq("nrd", { a(ea_register) }, !is_byte_access));
									op(Action::PerformOperation, seq("np nw", { a(ea_register) }, !is_byte_access));
									if(ea_mode == PostInc) {
										op(inc(ea_register) | MicroOp::DestinationMask);
									}
								break;

								case l(Ind):		// [CLR/NEG/NEGX/NOT].l (An)
								case l(PostInc):	// [CLR/NEG/NEGX/NOT].l (An)+
									op(int(Action::CopyToEffectiveAddress) | MicroOp::DestinationMask, seq("nRd+ nrd", { ea(1), ea(1) }));
									op(Action::PerformOperation, seq("np nw- nW", { ea(1), ea(1) }));
									if(ea_mode == PostInc) {
										op(int(Action::Increment4) | MicroOp::DestinationMask);
									}
								break;

								case bw(PreDec):	// [CLR/NEG/NEGX/NOT].bw -(An)
									op(	dec(ea_register) | MicroOp::DestinationMask,
										seq("nrd", { a(ea_register) }, !is_byte_access));
									op(Action::PerformOperation, seq("np nw", { a(ea_register) }, !is_byte_access));
								break;

								case l(PreDec):		// [CLR/NEG/NEGX/NOT].l -(An)
									op(int(Action::Decrement4) | MicroOp::DestinationMask);
									op(int(Action::CopyToEffectiveAddress) | MicroOp::DestinationMask,
										seq("n nRd+ nrd", { ea(1), ea(1) }));
									op(Action::PerformOperation, seq("np nw- nW", { ea(1), ea(1) }));
								break;

								case bw(d16An):		// [CLR/NEG/NEGX/NOT].bw (d16, An)
								case bw(d8AnXn):	// [CLR/NEG/NEGX/NOT].bw (d8, An, Xn)
									op(	calc_action_for_mode(mode) | MicroOp::DestinationMask,
										seq(pseq("np nrd", mode), { ea(1) },
										!is_byte_access));
									op(Action::PerformOperation, seq("np nw", { ea(1) }, !is_byte_access));
								break;

								case l(d16An):		// [CLR/NEG/NEGX/NOT].l (d16, An)
								case l(d8AnXn):		// [CLR/NEG/NEGX/NOT].l (d8, An, Xn)
									op(	calc_action_for_mode(mode) | MicroOp::DestinationMask,
										seq(pseq("np nRd+ nrd", mode), { ea(1), ea(1) }));
									op(Action::PerformOperation, seq("np nw- nW", { ea(1), ea(1) }));
								break;

								case bw(XXXl):		// [CLR/NEG/NEGX/NOT].bw (xxx).l
									op(Action::None, seq("np"));
								case bw(XXXw):		// [CLR/NEG/NEGX/NOT].bw (xxx).w
									op(	address_assemble_for_mode(mode) | MicroOp::DestinationMask,
										seq("np nrd", { ea(1) }, !is_byte_access));
									op(Action::PerformOperation,
										seq("np nw", { ea(1) }, !is_byte_access));
								break;

								case l(XXXl):		// [CLR/NEG/NEGX/NOT].l (xxx).l
									op(Action::None, seq("np"));
								case l(XXXw):		// [CLR/NEG/NEGX/NOT].l (xxx).w
									op(	address_assemble_for_mode(mode) | MicroOp::DestinationMask,
										seq("np nRd+ nrd", { ea(1), ea(1) }));
									op(Action::PerformOperation,
										seq("np nw- nW", { ea(1), ea(1) }));
								break;
							}
						} break;

						case Decoder::CMP: {
							const auto source_register = (instruction >> 9) & 7;

							storage_.instructions[instruction].destination = &storage_.data_[source_register];
							storage_.instructions[instruction].set_source(storage_, ea_mode, ea_register);

							// Byte accesses are not allowed with address registers.
							if(is_byte_access && ea_mode == An) {
								continue;
							}

							const int mode = combined_mode(ea_mode, ea_register);
							switch(is_long_word_access ? l(mode) : bw(mode)) {
								default: continue;

								case bw(Dn):		// CMP.bw Dn, Dn
								case l(Dn):			// CMP.l Dn, Dn
								case bw(An):		// CMP.w An, Dn
								case l(An):			// CMP.l An, Dn
									op(Action::PerformOperation, seq("np"));
								break;

								case bw(Ind):		// CMP.bw (An), Dn
								case bw(PostInc):	// CMP.bw (An)+, Dn
									op(Action::None, seq("nr np", { a(ea_register) }, !is_byte_access));
									if(mode == PostInc) {
										op(inc(ea_register) | MicroOp::SourceMask);
									}
									op(Action::PerformOperation);
								break;

								case l(Ind):		// CMP.l (An), Dn
								case l(PostInc):	// CMP.l (An)+, Dn
									op(int(Action::CopyToEffectiveAddress) | MicroOp::SourceMask, seq("nR+ nr np n", { ea(0), ea(0) }));
									if(mode == PostInc) {
										op(int(Action::Increment4) | MicroOp::SourceMask);
									}
									op(Action::PerformOperation);
								break;

								case bw(PreDec):	// CMP.bw -(An), Dn
									op(	dec(ea_register) | MicroOp::SourceMask,
										seq("n nr np", { a(ea_register) }, !is_byte_access));
									op(Action::PerformOperation);
								break;

								case l(PreDec):		// CMP.l -(An), Dn
									op(int(Action::Decrement4) | MicroOp::SourceMask);
									op(int(Action::CopyToEffectiveAddress) | MicroOp::SourceMask, seq("n nR+ nr np n", { ea(0), ea(0) }));
									op(Action::PerformOperation);
								break;

								case bw(d16An):		// CMP.bw (d16, An), Dn
								case bw(d8AnXn):	// CMP.bw (d8, An, Xn), Dn
								case bw(d16PC):		// CMP.bw (d16, PC), Dn
								case bw(d8PCXn):	// CMP.bw (d8, PC, Xn), Dn
									op(	calc_action_for_mode(mode) | MicroOp::SourceMask,
										seq(pseq("np nr np", mode), { ea(0) },
										!is_byte_access));
									op(Action::PerformOperation);
								break;

								case l(d16An):		// CMP.l (d16, An), Dn
								case l(d8AnXn):		// CMP.l (d8, An, Xn), Dn
								case l(d16PC):		// CMP.l (d16, PC), Dn
								case l(d8PCXn):		// CMP.l (d8, PC, Xn), Dn
									op(	calc_action_for_mode(mode) | MicroOp::SourceMask,
										seq(pseq("np nR+ nr np n", mode), { ea(0), ea(0) }));
									op(Action::PerformOperation);
								break;

								case bw(XXXl):		// CMP.bw (xxx).l, Dn
									op(Action::None, seq("np"));
								case bw(XXXw):		// CMP.bw (xxx).w, Dn
									op(	address_assemble_for_mode(mode) | MicroOp::SourceMask,
										seq("np nr np", { ea(0) }, !is_byte_access));
									op(Action::PerformOperation);
								break;

								case l(XXXl):		// CMP.l (xxx).l, Dn
									op(Action::None, seq("np"));
								case l(XXXw):		// CMP.l (xxx).w, Dn
									op(	address_assemble_for_mode(mode) | MicroOp::SourceMask,
										seq("np nR+ nr np n", { ea(0), ea(0) }));
									op(Action::PerformOperation);
								break;

								case bw(Imm):		// CMP.br #, Dn
									storage_.instructions[instruction].source = &storage_.prefetch_queue_;
									op(Action::PerformOperation, seq("np np"));
								break;

								case l(Imm):		// CMP.l #, Dn
									storage_.instructions[instruction].source = &storage_.prefetch_queue_;
									op(Action::None, seq("np"));
									op(Action::PerformOperation, seq("np np n"));
								break;
							}
						} break;

						case Decoder::CMPA: {
							// Only operation modes 011 and 111 are accepted, and long words are selected
							// by the top bit.
							if(((op_mode)&3) != 3) continue;
							is_long_word_access = op_mode == 7;

							const int destination_register = (instruction >> 9) & 7;

							storage_.instructions[instruction].set_source(storage_, ea_mode, ea_register);
							storage_.instructions[instruction].destination = &storage_.address_[destination_register];

							const int mode = combined_mode(ea_mode, ea_register, true);
							switch(is_long_word_access ? l(mode) : bw(mode)) {
								default: continue;

								case bw(Dn):		// CMPA.w [An/Dn], An
								case l(Dn):			// CMPA.l [An/Dn], An
									op(Action::PerformOperation, seq("np n"));
								break;

								case bw(Ind):		// CMPA.w (An), An
								case bw(PostInc):	// CMPA.w (An)+, An
									op(Action::None, seq("nr", { a(ea_register) }));
									op(Action::PerformOperation, seq("np n"));
									if(mode == PostInc) {
										op(int(Action::Increment2) | MicroOp::SourceMask);
									}
								break;

								case l(Ind):		// CMPA.l (An), An
								case l(PostInc):	// CMPA.l (An)+, An
									op(int(Action::CopyToEffectiveAddress) | MicroOp::SourceMask, seq("nR+ nr", { ea(0), ea(0) }));
									op(Action::PerformOperation, seq("np n"));
									if(mode == PostInc) {
										op(int(Action::Increment4) | MicroOp::SourceMask);
									}
								break;

								case bw(PreDec):	// CMPA.w -(An), An
									op(int(Action::Decrement2) | MicroOp::SourceMask, seq("n nr", { a(ea_register) }));
									op(Action::PerformOperation, seq("np n"));
								break;

								case l(PreDec):		// CMPA.l -(An), An
									op(int(Action::Decrement4) | MicroOp::SourceMask);
									op(int(Action::CopyToEffectiveAddress) | MicroOp::SourceMask, seq("nR+ nr", { ea(0), ea(0) }));
									op(Action::PerformOperation, seq("np n"));
								break;

								case bw(XXXl):		// CMPA.w (xxx).l, An
									op(Action::None, seq("np"));
								case bw(XXXw):		// CMPA.w (xxx).w, An
								case bw(d16PC):		// CMPA.w (d16, PC), An
								case bw(d8PCXn):	// CMPA.w (d8, PC, Xn), An
								case bw(d16An):		// CMPA.w (d16, An), An
								case bw(d8AnXn):	// CMPA.w (d8, An, Xn), An
									op(address_action_for_mode(mode) | MicroOp::SourceMask, seq(pseq("np nr", mode), { ea(0) }));
									op(Action::PerformOperation, seq("np n"));
								break;

								case l(XXXl):		// CMPA.l (xxx).l, An
									op(Action::None, seq("np"));
								case l(XXXw):		// CMPA.l (xxx).w, An
								case l(d16PC):		// CMPA.l (d16, PC), An
								case l(d8PCXn):		// CMPA.l (d8, PC, Xn), An
								case l(d16An):		// CMPA.l (d16, An), An
								case l(d8AnXn):		// CMPA.l (d8, An, Xn), An
									op(address_action_for_mode(mode) | MicroOp::SourceMask, seq(pseq("np nR+ nr", mode), { ea(0), ea(0) }));
									op(Action::PerformOperation, seq("np n"));
								break;

								case bw(Imm):		// CMPA.w #, An
									storage_.instructions[instruction].source = &storage_.prefetch_queue_;
									op(Action::PerformOperation, seq("np np n"));
								break;

								case l(Imm):		// CMPA.l #, An
									storage_.instructions[instruction].source = &storage_.prefetch_queue_;
									op(Action::None, seq("np"));
									op(Action::PerformOperation, seq("np np n"));
								break;
							}
						} break;

						case Decoder::CMPI: {
							if(ea_mode == An) continue;

							const auto destination_mode = ea_mode;
							const auto destination_register = ea_register;

							storage_.instructions[instruction].source = &storage_.source_bus_data_[0];
							storage_.instructions[instruction].set_destination(storage_, destination_mode, destination_register);

							const int mode = combined_mode(destination_mode, destination_register);
							switch(is_long_word_access ? l(mode) : bw(mode)) {
								default: continue;

								case bw(Dn):		// CMPI.bw #, Dn
									storage_.instructions[instruction].source = &storage_.prefetch_queue_;
									op(Action::PerformOperation, seq("np np"));
								break;

								case l(Dn):			// CMPI.l #, Dn
									storage_.instructions[instruction].source = &storage_.prefetch_queue_;
									op(Action::None, seq("np"));
									op(Action::PerformOperation, seq("np np n"));
								break;

								case bw(Ind):		// CMPI.bw #, (An)
								case bw(PostInc):	// CMPI.bw #, (An)+
									op(int(Action::AssembleWordDataFromPrefetch) | MicroOp::SourceMask, seq("np nrd np", { a(destination_register) }, !is_byte_access));
									if(mode == PostInc) {
										op(inc(destination_register) | MicroOp::DestinationMask);
									}
									op(Action::PerformOperation);
								break;

								case l(Ind):		// CMPI.l #, (An)
								case l(PostInc):	// CMPI.l #, (An)+
									op(int(Action::CopyToEffectiveAddress) | MicroOp::DestinationMask, seq("np"));
									op(int(Action::AssembleLongWordDataFromPrefetch) | MicroOp::SourceMask, seq("np nRd+ nrd np", { ea(1), ea(1) }));
									if(mode == PostInc) {
										op(int(Action::Increment4) | MicroOp::DestinationMask);
									}
									op(Action::PerformOperation);
								break;

								case bw(PreDec):	// CMPI.bw #, -(An)
									op(int(Action::AssembleWordDataFromPrefetch) | MicroOp::SourceMask, seq("np n"));
									op(dec(destination_register) | MicroOp::DestinationMask, seq("nrd np", { a(destination_register) }, !is_byte_access));
									op(Action::PerformOperation);
								break;

								case l(PreDec):		// CMPI.l #, -(An)
									op(int(Action::Decrement4) | MicroOp::DestinationMask, seq("np"));
									op(int(Action::AssembleLongWordDataFromPrefetch) | MicroOp::SourceMask, seq("np n"));
									op(int(Action::CopyToEffectiveAddress) | MicroOp::DestinationMask, seq("nRd+ nrd np", { ea(1), ea(1) }));
									op(Action::PerformOperation);
								break;

								case bw(d16An):		// CMPI.bw #, (d16, An)
								case bw(d8AnXn):	// CMPI.bw #, (d8, An, Xn)
									op(int(Action::AssembleWordDataFromPrefetch) | MicroOp::SourceMask, seq("np"));
									op(	calc_action_for_mode(mode) | MicroOp::DestinationMask,
										seq(pseq("np nrd np", mode), { ea(1) }, !is_byte_access));
									op(Action::PerformOperation);
								break;

								case l(d16An):		// CMPI.l #, (d16, An)
								case l(d8AnXn):		// CMPI.l #, (d8, An, Xn)
									op(int(Action::CopyToEffectiveAddress) | MicroOp::DestinationMask, seq("np"));
									op(int(Action::AssembleLongWordDataFromPrefetch) | MicroOp::SourceMask, seq("np"));
									op(	calc_action_for_mode(mode) | MicroOp::DestinationMask,
										seq(pseq("np nRd+ nrd np", mode), { ea(1), ea(1) }));
									op(Action::PerformOperation);
								break;

								case bw(XXXw):		// CMPI.bw #, (xxx).w
									op(int(Action::AssembleWordDataFromPrefetch) | MicroOp::SourceMask, seq("np"));
									op(int(Action::AssembleWordAddressFromPrefetch) | MicroOp::DestinationMask, seq("np nrd np",  { ea(1) }, !is_byte_access));
									op(Action::PerformOperation);
								break;

								case l(XXXw):		// CMPI.l #, (xxx).w
									op(Action::None, seq("np"));
									op(int(Action::AssembleLongWordDataFromPrefetch) | MicroOp::SourceMask, seq("np np"));
									op(int(Action::AssembleWordAddressFromPrefetch) | MicroOp::DestinationMask, seq("nRd+ nrd np",  { ea(1), ea(1) }));
									op(Action::PerformOperation);
								break;

								case bw(XXXl):		// CMPI.bw #, (xxx).l
									op(int(Action::AssembleWordDataFromPrefetch) | MicroOp::SourceMask, seq("np np"));
									op(int(Action::AssembleLongWordAddressFromPrefetch) | MicroOp::DestinationMask, seq("np nrd np",  { ea(1) }, !is_byte_access));
									op(Action::PerformOperation);
								break;

								case l(XXXl):		// CMPI.l #, (xxx).l
									op(Action::None, seq("np"));
									op(int(Action::AssembleLongWordDataFromPrefetch) | MicroOp::SourceMask, seq("np np"));
									op(int(Action::AssembleLongWordAddressFromPrefetch) | MicroOp::DestinationMask, seq("np nRd+ nrd np",  { ea(1), ea(1) }));
									op(Action::PerformOperation);
								break;
							}
						} break;

						case Decoder::CMPM: {
							const int source_register = (instruction >> 9)&7;
							const int destination_register = ea_register;

							storage_.instructions[instruction].set_source(storage_, 1, source_register);
							storage_.instructions[instruction].set_destination(storage_, 1, destination_register);

							const bool is_byte_operation = operation == Operation::CMPw;

							switch(operation) {
								default: continue;

								case Operation::CMPb:	// CMPM.b, (An)+, (An)+
								case Operation::CMPw:	// CMPM.w, (An)+, (An)+
									op(Action::None, seq("nr nr np", {a(source_register), a(destination_register)}, !is_byte_operation));
									op(Action::PerformOperation);
									op(inc(destination_register) | MicroOp::SourceMask | MicroOp::DestinationMask);
								break;

								case Operation::CMPl:
									op(	int(Action::CopyToEffectiveAddress) | MicroOp::SourceMask | MicroOp::DestinationMask,
										seq("nR+ nr nRd+ nrd np", {ea(0), ea(0), ea(1), ea(1)}));
									op(Action::PerformOperation);
									op(int(Action::Increment4) | MicroOp::SourceMask | MicroOp::DestinationMask);
								break;
							}
						} break;

						case Decoder::Scc_DBcc: {
							if(ea_mode == 1) {
								// This is a DBcc. Decode as such.
								operation = Operation::DBcc;
								storage_.instructions[instruction].source = &storage_.data_[ea_register];

								// Jump straight into deciding what steps to take next,
								// which will be selected dynamically.
								op(Action::PerformOperation);
								op();
							} else {
								// This is an Scc.

								// Scc is inexplicably a read-modify-write operation.
								storage_.instructions[instruction].set_source(storage_, ea_mode, ea_register);
								storage_.instructions[instruction].set_destination(storage_, ea_mode, ea_register);

								const int mode = combined_mode(ea_mode, ea_register);
								switch(mode) {
									default: continue;

									 case Dn:
										op(Action::PerformOperation, seq("np"));
										// TODO: if condition true, an extra n.
									 break;

									 case Ind:
									 case PostInc:
										op(Action::PerformOperation, seq("nr np nw", { a(ea_register), a(ea_register) }, false));
										if(mode == PostInc) {
											op(inc(ea_register) | MicroOp::DestinationMask);
										}
									 break;

									 case PreDec:
										op(dec(ea_register) | MicroOp::DestinationMask);
										op(Action::PerformOperation, seq("n nr np nw", { a(ea_register), a(ea_register) }, false));
									 break;

									 case d16An:
									 case d8AnXn:
										op(calc_action_for_mode(mode) | MicroOp::DestinationMask, seq(pseq("np nrd", mode), { ea(1) } , false));
										op(Action::PerformOperation, seq("np nw", { ea(1) } , false));
									 break;

									 case XXXw:
										op(Action::None, seq("np"));
									 case XXXl:
										op(address_assemble_for_mode(mode) | MicroOp::DestinationMask, seq(pseq("np nrd", mode), { ea(1) } , false));
										op(Action::PerformOperation, seq("np nw", { ea(1) } , false));
									 break;
								}
							}

						} break;

						case Decoder::JSR: {
							storage_.instructions[instruction].source = &storage_.effective_address_[0];
							const int mode = combined_mode(ea_mode, ea_register);
							switch(mode) {
								default: continue;
								case Ind:		// JSR (An)
									storage_.instructions[instruction].source = &storage_.address_[ea_register];
									op(Action::PrepareJSR);
									op(Action::PerformOperation, seq("np nW+ nw np", { ea(1), ea(1) }));
								break;

								case d16PC:		// JSR (d16, PC)
								case d16An:		// JSR (d16, An)
									op(Action::PrepareJSR);
									op(calc_action_for_mode(mode) | MicroOp::SourceMask);
									op(Action::PerformOperation, seq("n np nW+ nw np", { ea(1), ea(1) }));
								break;

								case d8PCXn:	// JSR (d8, PC, Xn)
								case d8AnXn:	// JSR (d8, An, Xn)
									op(Action::PrepareJSR);
									op(calc_action_for_mode(mode) | MicroOp::SourceMask);
									op(Action::PerformOperation, seq("n nn np nW+ nw np", { ea(1), ea(1) }));
								break;

								case XXXl:		// JSR (xxx).L
									op(Action::None, seq("np"));
									op(Action::PrepareJSR);
									op(address_assemble_for_mode(mode) | MicroOp::SourceMask);
									op(Action::PerformOperation, seq("n np nW+ nw np", { ea(1), ea(1) }));
								break;

								case XXXw:		// JSR (xxx).W
									op(Action::PrepareJSR);
									op(address_assemble_for_mode(mode) | MicroOp::SourceMask);
									op(Action::PerformOperation, seq("n np nW+ nw np", { ea(1), ea(1) }));
								break;
							}
						} break;

						case Decoder::RTS: {
							storage_.instructions[instruction].source = &storage_.source_bus_data_[0];
							op(Action::PrepareRTS, seq("nU nu"));
							op(Action::PerformOperation, seq("np np"));
						} break;

						case Decoder::JMP: {
							storage_.instructions[instruction].source = &storage_.effective_address_[0];
							const int mode = combined_mode(ea_mode, ea_register);
							switch(mode) {
								default: continue;
								case Ind:		// JMP (An)
									storage_.instructions[instruction].source = &storage_.address_[ea_register];
									op(Action::PerformOperation, seq("np np"));
								break;

								case d16PC:		// JMP (d16, PC)
								case d16An:		// JMP (d16, An)
									op(calc_action_for_mode(mode) | MicroOp::SourceMask);
									op(Action::PerformOperation, seq("n np np"));
								break;

								case d8PCXn:	// JMP (d8, PC, Xn)
								case d8AnXn:	// JMP (d8, An, Xn)
									op(calc_action_for_mode(mode) | MicroOp::SourceMask);
									op(Action::PerformOperation, seq("n nn np np"));
								break;

								case XXXl:	// JMP (xxx).L
									op(Action::None, seq("np"));
									op(address_assemble_for_mode(mode) | MicroOp::SourceMask);
									op(Action::PerformOperation, seq("np np"));
								break;

								case XXXw:	// JMP (xxx).W
									op(address_assemble_for_mode(mode) | MicroOp::SourceMask);
									op(Action::PerformOperation, seq("n np np"));
								break;
							}
						} break;

						case Decoder::LEA: {
							const int destination_register = (instruction >> 9) & 7;
							storage_.instructions[instruction].set_destination(storage_, An, destination_register);

							const int mode = combined_mode(ea_mode, ea_register);
							storage_.instructions[instruction].source_address = &storage_.address_[ea_register];
							storage_.instructions[instruction].source =
								(mode == Ind) ?
									&storage_.address_[ea_register] :
									&storage_.effective_address_[0];

							switch(mode) {
								default: continue;
								case Ind:		// LEA (An), An		(i.e. MOVEA)
									op(Action::PerformOperation, seq("np"));
								break;

								case d16An:		// LEA (d16, An), An
								case d16PC:		// LEA (d16, PC), An
									op(calc_action_for_mode(mode) | MicroOp::SourceMask, seq("np np"));
									op(Action::PerformOperation);
								break;

								case d8AnXn:	// LEA (d8, An, Xn), An
								case d8PCXn:	// LEA (d8, PC, Xn), An
									op(calc_action_for_mode(mode) | MicroOp::SourceMask, seq("n np n np"));
									op(Action::PerformOperation);
								break;

								case XXXl:	// LEA (xxx).L, An
									op(Action::None, seq("np"));
								case XXXw:		// LEA (xxx).W, An
									op(address_assemble_for_mode(mode) | MicroOp::SourceMask, seq("np np"));
									op(Action::PerformOperation);
								break;
							}
						} break;

						case Decoder::MOVEfromSR: {
							storage_.instructions[instruction].set_destination(storage_, ea_mode, ea_register);
							storage_.instructions[instruction].requires_supervisor = true;

							const int mode = combined_mode(ea_mode, ea_register);
							switch(mode) {
								default: continue;

								case Dn:		// MOVE SR, Dn
									op(Action::PerformOperation, seq("np n"));
								break;

								// NOTE ON nr BELOW.
								// It appears the 68000 performs a read-modify-write for this operation even
								// though it doesn't use the read; therefore where it's easier I've left the
								// nr within the same set of bus steps, before the PerformOperation, as it's
								// then a harmless read.
								//
								// DO NOT CORRECT TO nrd.

								case Ind:		// MOVE SR, (An)
								case PostInc:	// MOVE SR, (An)+
									op(Action::PerformOperation, seq("nr np nw", { a(ea_register), a(ea_register) }));
									if(mode == PostInc) {
										op(int(Action::Increment2) | MicroOp::DestinationMask);
									}
								break;

								case PreDec:	// MOVE SR, -(An)
									op(int(Action::Decrement2) | MicroOp::DestinationMask);
									op(Action::PerformOperation, seq("n nr np nw", { a(ea_register), a(ea_register) }));
								break;

								case XXXl:		// MOVE SR, (xxx).l
									op(Action::None, seq("np"));
								case XXXw:		// MOVE SR, (xxx).w
								case d16An:		// MOVE SR, (d16, An)
								case d8AnXn:	// MOVE SR, (d8, An, Xn)
									op(address_action_for_mode(mode) | MicroOp::DestinationMask, seq(pseq("np nr", mode), { ea(1) }));
									op(Action::PerformOperation, seq("np nw", { ea(1) }));
								break;
							}
						} break;

						case Decoder::MOVEtoSRCCR: {
							if(ea_mode == An) continue;
							storage_.instructions[instruction].set_source(storage_, ea_mode, ea_register);
							storage_.instructions[instruction].requires_supervisor = (operation == Operation::MOVEtoSR);

							/* DEVIATION FROM YACHT.TXT: it has all of these reading an extra word from the PC;
							this looks like a mistake so I've padded with nil cycles in the middle. */
							const int mode = combined_mode(ea_mode, ea_register);
							switch(mode) {
								default: continue;

								case Dn:		// MOVE Dn, SR
									op(Action::PerformOperation, seq("nn np"));
								break;

								case Ind:		// MOVE (An), SR
								case PostInc:	// MOVE (An)+, SR
									op(Action::None, seq("nr nn nn np", { a(ea_register) }));
									if(mode == PostInc) {
										op(int(Action::Increment2) | MicroOp::SourceMask);
									}
									op(Action::PerformOperation);
								break;

								case PreDec:	// MOVE -(An), SR
									op(Action::Decrement2, seq("n nr nn nn np", { a(ea_register) }));
									op(Action::PerformOperation);
								break;

								case d16PC:		// MOVE (d16, PC), SR
								case d8PCXn:	// MOVE (d8, PC, Xn), SR
								case d16An:		// MOVE (d16, An), SR
								case d8AnXn:	// MOVE (d8, An, Xn), SR
									op(calc_action_for_mode(mode) | MicroOp::SourceMask, seq(pseq("np nr nn nn np", mode), { ea(0) }));
									op(Action::PerformOperation);
								break;

								case XXXl:	// MOVE (xxx).L, SR
									op(Action::None, seq("np"));
								case XXXw:	// MOVE (xxx).W, SR
									op(
										address_assemble_for_mode(mode) | MicroOp::SourceMask,
										seq("np nr nn nn np", { ea(0) }));
									op(Action::PerformOperation);
								break;

								case Imm:	// MOVE #, SR
									storage_.instructions[instruction].source = &storage_.prefetch_queue_;
									op(int(Action::PerformOperation), seq("np nn nn np"));
								break;
							}
						} break;

						case Decoder::MOVEq: {
							const int destination_register = (instruction >> 9) & 7;
							storage_.instructions[instruction].destination = &storage_.data_[destination_register];
							op(Action::PerformOperation, seq("np"));
						} break;

						case Decoder::MOVEM: {
							// For the sake of commonality, both to R and to M will evaluate their addresses
							// as if they were destinations.
							storage_.instructions[instruction].set_destination(storage_, ea_mode, ea_register);

							// Standard prefix: acquire the register selection flags and fetch the next program
							// word to replace them.
							op(Action::CopyNextWord, seq("np"));

							// Do whatever is necessary to calculate the proper start address.
							const int mode = combined_mode(ea_mode, ea_register);
							const bool is_to_m = (operation == Operation::MOVEMtoMl || operation == Operation::MOVEMtoMw);
							switch(mode) {
								default: continue;

								case Ind:
								case PreDec:
								case PostInc: {
									// Deal with the illegal combinations.
									if(mode == PostInc && is_to_m) continue;
									if(mode == PreDec && !is_to_m) continue;
								} break;

								case d16An:
								case d8AnXn:
								case d16PC:
								case d8PCXn:
									// PC-relative addressing is permitted for moving to registers only.
									if((mode == d16PC || mode == d8PCXn) && is_to_m) continue;
									op(calc_action_for_mode(mode) | MicroOp::DestinationMask, seq(pseq("np", mode)));
								break;

								case XXXl:
									op(Action::None, seq("np"));
								case XXXw:
									op(address_assemble_for_mode(mode) | MicroOp::DestinationMask, seq("np"));
								break;
							}

							// Standard suffix: perform the MOVEM, which will mean evaluating the
							// register selection flags and substituting the necessary reads or writes.
							op(Action::PerformOperation);

							// A final program fetch will cue up the next instruction.
							op(is_to_m ? Action::MOVEMtoMComplete : Action::MOVEMtoRComplete, seq("np"));
						} break;

						case Decoder::MOVEUSP: {
							storage_.instructions[instruction].requires_supervisor = true;

							// Observation here: because this is a privileged instruction, the user stack pointer
							// definitely isn't currently [copied into] A7.
							if(instruction & 0x8) {
								// Transfer FROM the USP.
								storage_.instructions[instruction].source = &storage_.stack_pointers_[0];
								storage_.instructions[instruction].set_destination(storage_, An, ea_register);
							} else {
								// Transfer TO the USP.
								storage_.instructions[instruction].set_source(storage_, An, ea_register);
								storage_.instructions[instruction].destination = &storage_.stack_pointers_[0];
							}

							op(Action::PerformOperation, seq("np"));
						} break;

						// Decodes the format used by most MOVEs and all MOVEAs.
						case Decoder::MOVE: {
							const int destination_mode = (instruction >> 6) & 7;
							const int destination_register = (instruction >> 9) & 7;

							storage_.instructions[instruction].set_source(storage_, ea_mode, ea_register);
							storage_.instructions[instruction].set_destination(storage_, destination_mode, destination_register);

							// These don't come from the usual place.
							is_byte_access = mapping.operation == Operation::MOVEb;
							is_long_word_access = mapping.operation == Operation::MOVEl;

							const int combined_source_mode = combined_mode(ea_mode, ea_register, true, true);
							const int combined_destination_mode = combined_mode(destination_mode, destination_register, true, true);
							const int mode = is_long_word_access ?
								l2(combined_source_mode, combined_destination_mode) :
								bw2(combined_source_mode, combined_destination_mode);

							// If the move is to an address register, switch the MOVE to a MOVEA.
							// Also: there are no byte moves to address registers.
							if(destination_mode == An) {
								if(is_byte_access) {
									continue;
								}
								operation = is_long_word_access ? Operation::MOVEAl : Operation::MOVEAw;
							}

							// ... there are also no byte moves from address registers.
							if(ea_mode == An && is_byte_access) continue;

							switch(mode) {

							//
							// MOVE <ea>, Dn
							// MOVEA <ea>, An
							//

								case l2(Dn, Dn):		// MOVE[A].l [An/Dn], [An/Dn]
								case bw2(Dn, Dn):		// MOVE[A].bw [An/Dn], [An/Dn]
									op(Action::PerformOperation, seq("np"));
								break;

								case l2(PreDec, Dn):	// MOVE[A].l -(An), [An/Dn]
									op(dec(ea_register) | MicroOp::SourceMask, seq("n"));
								case l2(Ind, Dn):		// MOVE[A].l (An)[+], [An/Dn]
									op(int(Action::CopyToEffectiveAddress) | MicroOp::SourceMask, seq("nR+ nr np", { ea(0), ea(0) }));
									op(Action::PerformOperation);
								break;

								case bw2(Ind, Dn):		// MOVE[A].bw (An)[+], [An/Dn]
									op(Action::None, seq("nr np", { a(ea_register) }, !is_byte_access));
									op(Action::PerformOperation);
								break;

								case bw2(PreDec, Dn):	// MOVE[A].bw -(An), [An/Dn]
									op(dec(ea_register) | MicroOp::SourceMask, seq("n nr np", { a(ea_register) }, !is_byte_access));
									op(Action::PerformOperation);
								break;

								case l2(XXXl, Dn):		// MOVE[A].l (xxx).L, [An/Dn]
									op(Action::None, seq("np"));
								case l2(XXXw, Dn):		// MOVE[A].l (xxx).W, [An/Dn]
								case l2(d16An, Dn):		// MOVE[A].l (d16, An), [An/Dn]
								case l2(d8AnXn, Dn):	// MOVE[A].l (d8, An, Xn), [An/Dn]
								case l2(d16PC, Dn):		// MOVE[A].l (d16, PC), [An/Dn]
								case l2(d8PCXn, Dn):	// MOVE[A].l (d8, PC, Xn), [An/Dn]
									op(	address_action_for_mode(combined_source_mode) | MicroOp::SourceMask,
										seq(pseq("np nR+ nr np", combined_source_mode), { ea(0), ea(0) }));
									op(Action::PerformOperation);
								break;

								case bw2(XXXl, Dn):		// MOVE[A].bw (xxx).L, [An/Dn]
									op(Action::None, seq("np"));
								case bw2(XXXw, Dn):		// MOVE[A].bw (xxx).W, [An/Dn]
								case bw2(d16An, Dn):	// MOVE[A].bw (d16, An), [An/Dn]
								case bw2(d8AnXn, Dn):	// MOVE[A].bw (d8, An, Xn), [An/Dn]
								case bw2(d16PC, Dn):	// MOVE[A].bw (d16, PC), [An/Dn]
								case bw2(d8PCXn, Dn):	// MOVE[A].bw (d8, PC, Xn), [An/Dn]
									op(	address_action_for_mode(combined_source_mode) | MicroOp::SourceMask,
										seq(pseq("np nr np", combined_source_mode), { ea(0) },
										!is_byte_access));
									op(Action::PerformOperation);
								break;

								case l2(Imm, Dn):		// MOVE[A].l #, [An/Dn]
									storage_.instructions[instruction].source = &storage_.prefetch_queue_;
									op(Action::None, seq("np"));
									op(int(Action::PerformOperation), seq("np np"));
								break;

								case bw2(Imm, Dn):		// MOVE[A].bw #, [An/Dn]
									storage_.instructions[instruction].source = &storage_.prefetch_queue_;
									op(int(Action::PerformOperation), seq("np np"));
								break;

							//
							// MOVE <ea>, (An)[+]
							//

								case l2(Dn, Ind):			// MOVE.l [An/Dn], (An)[+]
									op(int(Action::CopyToEffectiveAddress) | MicroOp::DestinationMask);
									op(Action::PerformOperation, seq("nW+ nw np", { ea(1), ea(1) }));
								break;

								case bw2(Dn, Ind):			// MOVE.bw [An/Dn], (An)[+]
									op(Action::PerformOperation, seq("nw np", { a(destination_register) }, !is_byte_access));
								break;

								case l2(PreDec, Ind):		// MOVE.l -(An), (An)[+]
									op(dec(ea_register) | MicroOp::SourceMask, seq("n"));
								case l2(Ind, Ind):			// MOVE.l (An)[+], (An)[+]
									op(	int(Action::CopyToEffectiveAddress) | MicroOp::DestinationMask | MicroOp::SourceMask,
										seq("nR+ nr", { ea(0), ea(0) }));
									op(Action::PerformOperation, seq("nW+ nw np", { ea(1), ea(1) }));
								break;

								case bw2(Ind, Ind):			// MOVE.bw (An)[+], (An)[+]
									op(Action::None, seq("nr", { a(ea_register) }, !is_byte_access));
									op(Action::PerformOperation, seq("nw np", { a(destination_register) }, !is_byte_access));
								break;

								case bw2(PreDec, Ind):		// MOVE.bw -(An), (An)[+]
									op(dec(ea_register) | MicroOp::SourceMask, seq("n nr", { a(ea_register) }, !is_byte_access));
									op(Action::PerformOperation, seq("nw np", { a(destination_register) }, !is_byte_access));
								break;

								case l2(d16An, Ind):		// MOVE.bw (d16, An), (An)[+]
								case l2(d8AnXn, Ind):		// MOVE.bw (d8, An, Xn), (An)[+]
								case l2(d16PC, Ind):		// MOVE.bw (d16, PC), (An)[+]
								case l2(d8PCXn, Ind):		// MOVE.bw (d8, PC, Xn), (An)[+]
									op( int(Action::CopyToEffectiveAddress) | MicroOp::DestinationMask);
									op(	calc_action_for_mode(combined_source_mode) | MicroOp::SourceMask,
										seq(pseq("np nR+ nr", combined_source_mode), { ea(0), ea(0) }));
									op(	Action::PerformOperation,
										seq("nW+ nw np", { ea(1), ea(1) }));
								break;

								case bw2(XXXl, Ind):		// MOVE.bw (xxx).l, (An)[+]
									op(	Action::None, seq("np"));
								case bw2(XXXw, Ind):		// MOVE.bw (xxx).W, (An)[+]
								case bw2(d16An, Ind):		// MOVE.bw (d16, An), (An)[+]
								case bw2(d8AnXn, Ind):		// MOVE.bw (d8, An, Xn), (An)[+]
								case bw2(d16PC, Ind):		// MOVE.bw (d16, PC), (An)[+]
								case bw2(d8PCXn, Ind):		// MOVE.bw (d8, PC, Xn), (An)[+]
									op(	address_action_for_mode(combined_source_mode) | MicroOp::SourceMask,
										seq(pseq("np nr", combined_source_mode), { ea(0) }, !is_byte_access));
									op(	Action::PerformOperation,
										seq("nw np", { a(destination_register) }, !is_byte_access));
								break;

								case l2(XXXl, Ind):			// MOVE.l (xxx).l, (An)[+]
								case l2(XXXw, Ind):			// MOVE.l (xxx).W, (An)[+]
									op(	int(Action::CopyToEffectiveAddress) | MicroOp::DestinationMask,
									 	(combined_source_mode == XXXl) ? seq("np") : nullptr);
									op(	address_assemble_for_mode(combined_source_mode) | MicroOp::SourceMask,
										seq("np nR+ nr", { ea(0), ea(0) }));
									op(	Action::PerformOperation,
										seq("nW+ nw np", { ea(1), ea(1) }));
								break;

								case l2(Imm, Ind):			// MOVE.l #, (An)[+]
									storage_.instructions[instruction].source = &storage_.prefetch_queue_;
									op( int(Action::CopyToEffectiveAddress) | MicroOp::DestinationMask, seq("np") );
									op(	Action::PerformOperation, seq("np nW+ nw np", { ea(1), ea(1) }) );
								break;

								case bw2(Imm, Ind):			// MOVE.bw #, (An)[+]
									storage_.instructions[instruction].source = &storage_.prefetch_queue_;
									op(Action::PerformOperation, seq("np nw np", { a(destination_register) }, !is_byte_access) );
								break;

							//
							// MOVE <ea>, -(An)
							//

								case bw2(Dn, PreDec):		// MOVE.bw [An/Dn], -(An)
									op(Action::PerformOperation);
									op(	dec(destination_register) | MicroOp::DestinationMask,
										seq("np nw", { a(destination_register) }, !is_byte_access));
								break;

								case l2(Dn, PreDec):		// MOVE.l [An/Dn], -(An)
									op(Action::PerformOperation);
									op( int(Action::Decrement2) | MicroOp::DestinationMask, seq("np") );
									op( int(Action::CopyToEffectiveAddress) | MicroOp::DestinationMask, seq("nw- nW", { ea(1), ea(1) } ));
									op( int(Action::Decrement2) | MicroOp::DestinationMask );
								break;

								// TODO: verify when/where the predecrement occurs everywhere below; it may affect
								// e.g. MOVE.w -(A6), -(A6)

								case bw2(PreDec, PreDec):	// MOVE.bw -(An), -(An)
									op(dec(ea_register) | MicroOp::SourceMask, seq("n"));
								case bw2(Ind, PreDec):		// MOVE.bw (An)[+], -(An)
									op(dec(destination_register) | MicroOp::DestinationMask, seq("nr", { a(ea_register) }, !is_byte_access));
									op(Action::PerformOperation, seq("np nw", { a(destination_register) }, !is_byte_access));
								break;

								case l2(PreDec, PreDec):	// MOVE.l -(An), -(An)
									op(dec(ea_register) | MicroOp::SourceMask, seq("n"));
								case l2(Ind, PreDec):		// MOVE.l (An)[+], -(An)
									op(int(Action::CopyToEffectiveAddress) | MicroOp::SourceMask, seq("nR+ nr", { ea(0), ea(0) } ));
									op(Action::PerformOperation);
									op(int(Action::Decrement2) | MicroOp::DestinationMask, seq("np") );
									op(int(Action::CopyToEffectiveAddress) | MicroOp::DestinationMask, seq("nw- nW", { ea(1), ea(1) } ));
									op(int(Action::Decrement2) | MicroOp::DestinationMask );
								break;

								case bw2(XXXl, PreDec):		// MOVE.bw (xxx).l, -(An)
									op(Action::None, seq("np"));
								case bw2(XXXw, PreDec):		// MOVE.bw (xxx).w, -(An)
								case bw2(d16An, PreDec):	// MOVE.bw (d16, An), -(An)
								case bw2(d8AnXn, PreDec):	// MOVE.bw (d8, An, Xn), -(An)
								case bw2(d16PC, PreDec):	// MOVE.bw (d16, PC), -(An)
								case bw2(d8PCXn, PreDec):	// MOVE.bw (d8, PC, Xn), -(An)
									op(	address_action_for_mode(combined_source_mode) | MicroOp::SourceMask,
										seq(pseq("np nr", combined_source_mode), { ea(0) }, !is_byte_access ));
									op(Action::PerformOperation);
									op(dec(destination_register) | MicroOp::DestinationMask, seq("np nw", { a(destination_register) }, !is_byte_access));
								break;

								case l2(XXXl, PreDec):		// MOVE.l (xxx).w, -(An)
									op(Action::None, seq("np"));
								case l2(XXXw, PreDec):		// MOVE.l (xxx).l, -(An)
								case l2(d16An, PreDec):		// MOVE.l (d16, An), -(An)
								case l2(d8AnXn, PreDec):	// MOVE.l (d8, An, Xn), -(An)
								case l2(d16PC, PreDec):		// MOVE.l (d16, PC), -(An)
								case l2(d8PCXn, PreDec):	// MOVE.l (d8, PC, Xn), -(An)
									op(	address_action_for_mode(combined_source_mode) | MicroOp::SourceMask,
										seq(pseq("np nR+ nr", combined_source_mode), { ea(0), ea(0) } ));
									op(Action::PerformOperation);
									op(int(Action::Decrement2) | MicroOp::DestinationMask, seq("np") );
									op(int(Action::CopyToEffectiveAddress) | MicroOp::DestinationMask, seq("nw- nW", { ea(1), ea(1) }));
									op(int(Action::Decrement2) | MicroOp::DestinationMask);
								break;

								case bw2(Imm, PreDec):		// MOVE.bw #, -(An)
									op(int(Action::AssembleWordDataFromPrefetch) | MicroOp::SourceMask, seq("np"));
									op(Action::PerformOperation);
									op(dec(destination_register) | MicroOp::DestinationMask, seq("np nw", { a(destination_register) }, !is_byte_access));
								break;

								case l2(Imm, PreDec):		// MOVE.l #, -(An)
									op(Action::None, seq("np"));
									op(int(Action::AssembleLongWordDataFromPrefetch) | MicroOp::SourceMask, seq("np"));
									op(Action::PerformOperation);
									op(int(Action::Decrement2) | MicroOp::DestinationMask, seq("np") );
									op(int(Action::CopyToEffectiveAddress) | MicroOp::DestinationMask, seq("nw- nW", { ea(1), ea(1) }));
									op(int(Action::Decrement2) | MicroOp::DestinationMask);
								break;

							//
							// MOVE <ea>, (d16, An)
							// MOVE <ea>, (d8, An, Xn)
							// MOVE <ea>, (d16, PC)
							// MOVE <ea>, (d8, PC, Xn)
							//

#define bw2d(s)	bw2(s, d16An): case bw2(s, d8AnXn)
#define l2d(s)	l2(s, d16An): case l2(s, d8AnXn)

								case bw2d(Dn):				// MOVE.bw [An/Dn], (d16, An)/(d8, An, Xn)
									op(calc_action_for_mode(combined_destination_mode) | MicroOp::DestinationMask, seq(pseq("np", combined_destination_mode)));
									op(Action::PerformOperation, seq("nw np", { ea(1) }, !is_byte_access));
								break;

								case l2d(Dn):				// MOVE.l [An/Dn], (d16, An)/(d8, An, Xn)
									op(calc_action_for_mode(combined_destination_mode) | MicroOp::DestinationMask, seq(pseq("np", combined_destination_mode)));
									op(Action::PerformOperation, seq("nW+ nw np", { ea(1), ea(1) }));
								break;

								case bw2d(Ind):				// MOVE.bw (An)[+], (d16, An)/(d8, An, Xn)
									op(calc_action_for_mode(combined_destination_mode) | MicroOp::DestinationMask, seq("nr", { a(ea_register) }, !is_byte_access));
									op(Action::PerformOperation, seq(pseq("np nw np", combined_destination_mode), { ea(1) }, !is_byte_access));
								break;

								case l2d(Ind):				// MOVE.l (An)[+], (d16, An)/(d8, An, Xn)
									op(int(Action::CopyToEffectiveAddress) | MicroOp::SourceMask);
									op(calc_action_for_mode(combined_destination_mode) | MicroOp::DestinationMask, seq("nR+ nr", { ea(0), ea(0) }));
									op(Action::PerformOperation, seq(pseq("np nW+ nw np", combined_destination_mode), { ea(1), ea(1) }));
								break;

								case bw2d(PreDec):			// MOVE.bw -(An), (d16, An)/(d8, An, Xn)
									op(dec(ea_register) | MicroOp::SourceMask, seq("n nr", { a(ea_register) }, !is_byte_access));
									op(Action::PerformOperation);
									op(calc_action_for_mode(combined_destination_mode) | MicroOp::DestinationMask, seq("np nw np", { ea(1) }, !is_byte_access));
								break;

								case l2d(PreDec):			// MOVE.l -(An), (d16, An)/(d8, An, Xn)
									op(dec(ea_register) | MicroOp::SourceMask, seq("n"));
									op(int(Action::CopyToEffectiveAddress) | MicroOp::SourceMask, seq("nR+ nr", { ea(0), ea(0) }));
									op(Action::PerformOperation);
									op(calc_action_for_mode(combined_destination_mode) | MicroOp::DestinationMask, seq("np nW+ nw np", { ea(1), ea(1) }));
								break;

								case bw2d(XXXl):			// MOVE.bw (xxx).l, (d16, An)/(d8, An, Xn)
									op(Action::None, seq("np"));
								case bw2d(XXXw):			// MOVE.bw (xxx).w, (d16, An)/(d8, An, Xn)
								case bw2d(d16An):			// MOVE.bw (d16, An), (d16, An)/(d8, An, Xn)
								case bw2d(d8AnXn):			// MOVE.bw (d8, An, Xn), (d16, An)/(d8, An, Xn)
								case bw2d(d16PC):			// MOVE.bw (d16, PC), (d16, An)/(d8, An, Xn)
								case bw2d(d8PCXn):			// MOVE.bw (d8, PC, xn), (d16, An)/(d8, An, Xn)
									op(address_action_for_mode(combined_source_mode) | MicroOp::SourceMask, seq(pseq("np nr", combined_source_mode), { ea(0) }, !is_byte_access));
									op(Action::PerformOperation);
									op(calc_action_for_mode(combined_destination_mode) | MicroOp::DestinationMask, seq(pseq("np nw np", combined_destination_mode), { ea(1) }, !is_byte_access));
								break;

								case l2d(XXXl):				// MOVE.l (xxx).l, (d16, An)/(d8, An, Xn)
									op(Action::None, seq("np"));
								case l2d(XXXw):				// MOVE.l (xxx).w, (d16, An)/(d8, An, Xn)
								case l2d(d16An):			// MOVE.l (d16, An), (d16, An)/(d8, An, Xn)
								case l2d(d8AnXn):			// MOVE.l (d8, An, Xn), (d16, An)/(d8, An, Xn)
								case l2d(d16PC):			// MOVE.l (d16, PC), (d16, An)/(d8, An, Xn)
								case l2d(d8PCXn):			// MOVE.l (d8, PC, xn), (d16, An)/(d8, An, Xn)
									op(address_action_for_mode(combined_source_mode) | MicroOp::SourceMask, seq(pseq("np nR+ nr", combined_source_mode), { ea(0), ea(0) }));
									op(Action::PerformOperation);
									op(calc_action_for_mode(combined_destination_mode) | MicroOp::DestinationMask, seq(pseq("np nW+ nw np", combined_destination_mode), { ea(1), ea(1) }));
								break;

								case bw2d(Imm):				// MOVE.bw #, (d16, An)/(d8, An, Xn)
									op(int(Action::AssembleWordDataFromPrefetch) | MicroOp::SourceMask, seq("np"));
									op(calc_action_for_mode(combined_destination_mode) | MicroOp::DestinationMask, seq(pseq("np", combined_destination_mode)));
									op(Action::PerformOperation, seq("nw np", { ea(1) }, !is_byte_access));
								break;

								case l2d(Imm):				// MOVE.l #, (d16, An)/(d8, An, Xn)
									op(Action::None, seq("np"));
									op(int(Action::AssembleLongWordDataFromPrefetch) | MicroOp::SourceMask, seq("np"));
									op(calc_action_for_mode(combined_destination_mode) | MicroOp::DestinationMask, seq(pseq("np", combined_destination_mode)));
									op(Action::PerformOperation, seq("nW+ nw np", { ea(1), ea(1) }));
								break;

#undef bw2d
#undef l2d

							//
							// MOVE <ea>, (xxx).W
							// MOVE <ea>, (xxx).L
							//

								case bw2(Dn, XXXl):			// MOVE.bw [An/Dn], (xxx).L
									op(Action::None, seq("np"));
								case bw2(Dn, XXXw):			// MOVE.bw [An/Dn], (xxx).W
									op(address_assemble_for_mode(combined_destination_mode) | MicroOp::DestinationMask, seq("np"));
									op(Action::PerformOperation, seq("nw np", { ea(1) }, !is_byte_access));
								break;

								case l2(Dn, XXXl):			// MOVE.l [An/Dn], (xxx).L
									op(Action::None, seq("np"));
								case l2(Dn, XXXw):			// MOVE.l [An/Dn], (xxx).W
									op(address_assemble_for_mode(combined_destination_mode) | MicroOp::DestinationMask, seq("np"));
									op(Action::PerformOperation, seq("nW+ nw np", { ea(1), ea(1) }));
								break;

								case bw2(Ind, XXXw):		// MOVE.bw (An)[+], (xxx).W
									op(	address_assemble_for_mode(combined_destination_mode) | MicroOp::DestinationMask,
										seq("nr np", { a(ea_register) }, !is_byte_access));
									op(Action::PerformOperation, seq("nw np", { ea(1) }));
								break;

								case bw2(Ind, XXXl):		// MOVE.bw (An)[+], (xxx).L
									op(Action::None, seq("nr np", { a(ea_register) }, !is_byte_access));
									op(Action::PerformOperation, seq("nw np np", { &storage_.prefetch_queue_.full }));
								break;

								case l2(Ind, XXXw):			// MOVE.l (An)[+], (xxx).W
									op(	int(Action::CopyToEffectiveAddress) | MicroOp::SourceMask,
										seq("nR+ nr", { ea(0), ea(0) }));
									op(	address_assemble_for_mode(combined_destination_mode) | MicroOp::DestinationMask,
										seq("np", { a(ea_register) }, !is_byte_access));
									op(Action::PerformOperation, seq("nW+ nw np", { ea(1), ea(1) }));
								break;

								case l2(Ind, XXXl):			// MOVE (An)[+], (xxx).L
									op(int(Action::CopyToEffectiveAddress) | MicroOp::SourceMask, seq("nR+ nr np", { ea(0), ea(0) }));
									op(address_assemble_for_mode(combined_destination_mode));
									op(Action::PerformOperation, seq("nW+ nw np np", { ea(1), ea(1) }));
								break;

								case bw2(PreDec, XXXw):		// MOVE.bw -(An), (xxx).W
									op( dec(ea_register) | MicroOp::SourceMask);
									op(	address_assemble_for_mode(combined_destination_mode) | MicroOp::DestinationMask,
										seq("n nr np", { a(ea_register) }, !is_byte_access));
									op(Action::PerformOperation, seq("nw np", { ea(1) }, !is_byte_access));
								break;

								case bw2(PreDec, XXXl):		// MOVE.bw -(An), (xxx).L
									op(dec(ea_register) | MicroOp::SourceMask, seq("n nr np", { a(ea_register) }, !is_byte_access));
									op(address_assemble_for_mode(combined_destination_mode) | MicroOp::DestinationMask);
									op(Action::PerformOperation, seq("nw np np", { ea(1) }, !is_byte_access));
								break;

								case l2(PreDec, XXXw):		// MOVE.l -(An), (xxx).W
									op(	dec(ea_register) | MicroOp::SourceMask);
									op(	int(Action::CopyToEffectiveAddress) | MicroOp::SourceMask,
										seq("n nR+ nr", { ea(0), ea(0) } ));
									op(	address_assemble_for_mode(combined_destination_mode) | MicroOp::DestinationMask, seq("np"));
									op( Action::PerformOperation,
										seq("np nW+ nw np", { ea(1), ea(1) }));
								break;

								case l2(PreDec, XXXl):		// MOVE.l -(An), (xxx).L
									op(	dec(ea_register) | MicroOp::SourceMask);
									op(	int(Action::CopyToEffectiveAddress) | MicroOp::SourceMask,
										seq("n nR+ nr np", { ea(0), ea(0) } ));
									op(	address_assemble_for_mode(combined_destination_mode) | MicroOp::DestinationMask, seq("np"));
									op( Action::PerformOperation,
										seq("nW+ nw np np", { ea(1), ea(1) }));
								break;

								case bw2(d16PC, XXXw):		// MOVE.bw (d16, PC), (xxx).w
								case bw2(d16An, XXXw):		// MOVE.bw (d16, An), (xxx).w
								case bw2(d8PCXn, XXXw):		// MOVE.bw (d8, PC, Xn), (xxx).w
								case bw2(d8AnXn, XXXw):		// MOVE.bw (d8, An, Xn), (xxx).w
									op(calc_action_for_mode(combined_source_mode) | MicroOp::SourceMask, seq(pseq("np nr", combined_source_mode), { ea(0) }, !is_byte_access));
									op(Action::PerformOperation);
									op(int(Action::AssembleWordAddressFromPrefetch) | MicroOp::DestinationMask, seq("np nw np", { ea(1) }, !is_byte_access));
								break;

								case bw2(d16PC, XXXl):		// MOVE.bw (d16, PC), (xxx).l
								case bw2(d16An, XXXl):		// MOVE.bw (d16, An), (xxx).l
								case bw2(d8PCXn, XXXl):		// MOVE.bw (d8, PC, Xn), (xxx).l
								case bw2(d8AnXn, XXXl):		// MOVE.bw (d8, An, Xn), (xxx).l
									op(calc_action_for_mode(combined_source_mode) | MicroOp::SourceMask, seq(pseq("np np nr", combined_source_mode), { ea(0) }, !is_byte_access));
									op(Action::PerformOperation);
									op(int(Action::AssembleLongWordAddressFromPrefetch) | MicroOp::DestinationMask, seq("np nw np", { ea(1) }, !is_byte_access));
								break;

								case l2(d16PC, XXXw):		// MOVE.l (d16, PC), (xxx).w
								case l2(d16An, XXXw):		// MOVE.l (d16, An), (xxx).w
								case l2(d8PCXn, XXXw):		// MOVE.l (d8, PC, Xn), (xxx).w
								case l2(d8AnXn, XXXw):		// MOVE.l (d8, An, Xn), (xxx).w
									op(calc_action_for_mode(combined_source_mode) | MicroOp::SourceMask, seq(pseq("np nR+ nr", combined_source_mode), { ea(0), ea(0) }));
									op(Action::PerformOperation);
									op(int(Action::AssembleWordAddressFromPrefetch) | MicroOp::DestinationMask, seq("np nW+ nw np", { ea(1), ea(1) }));
								break;

								case l2(d16PC, XXXl):		// MOVE.l (d16, PC), (xxx).l
								case l2(d16An, XXXl):		// MOVE.l (d16, An), (xxx).l
								case l2(d8PCXn, XXXl):		// MOVE.l (d8, PC, Xn), (xxx).l
								case l2(d8AnXn, XXXl):		// MOVE.l (d8, An, Xn), (xxx).l
									op(calc_action_for_mode(combined_source_mode) | MicroOp::SourceMask, seq(pseq("np np nR+ nr", combined_source_mode), { ea(0), ea(0) }));
									op(Action::PerformOperation);
									op(int(Action::AssembleLongWordAddressFromPrefetch) | MicroOp::DestinationMask, seq("np nW+ nw np", { ea(1), ea(1) }));
								break;

								case bw2(Imm, XXXw):		// MOVE.bw #, (xxx).w
									op(int(Action::AssembleWordDataFromPrefetch) | MicroOp::DestinationMask, seq("np"));
									op(Action::PerformOperation);
									op(int(Action::AssembleWordAddressFromPrefetch) | MicroOp::DestinationMask, seq("np nw np", { ea(1) }, !is_byte_access));
								break;

								case bw2(Imm, XXXl):		// MOVE.bw #, (xxx).l
									storage_.instructions[instruction].source = &storage_.destination_bus_data_[0];
									op(int(Action::AssembleWordDataFromPrefetch) | MicroOp::DestinationMask, seq("np np"));
									op(Action::PerformOperation);
									op(int(Action::AssembleLongWordAddressFromPrefetch) | MicroOp::DestinationMask, seq("np nw np", { ea(1) }, !is_byte_access));
								break;

								case l2(Imm, XXXw):			// MOVE.l #, (xxx).w
									storage_.instructions[instruction].source = &storage_.destination_bus_data_[0];
									op(int(Action::None), seq("np"));
									op(int(Action::AssembleLongWordDataFromPrefetch) | MicroOp::DestinationMask, seq("np"));
									op(Action::PerformOperation);
									op(int(Action::AssembleWordAddressFromPrefetch) | MicroOp::DestinationMask, seq("np nW+ nw np", { ea(1), ea(1) }));
								break;

								case l2(Imm, XXXl):			// MOVE.l #, (xxx).l
									storage_.instructions[instruction].source = &storage_.destination_bus_data_[0];
									op(int(Action::None), seq("np"));
									op(int(Action::AssembleLongWordDataFromPrefetch) | MicroOp::DestinationMask, seq("np np"));
									op(Action::PerformOperation);
									op(int(Action::AssembleLongWordAddressFromPrefetch) | MicroOp::DestinationMask, seq("np nW+ nw np", { ea(1), ea(1) }));
								break;

								case bw2(XXXw, XXXw):		// MOVE.bw (xxx).w, (xxx).w
									op(int(Action::AssembleWordAddressFromPrefetch) | MicroOp::SourceMask, seq("np nr", { ea(0) }, !is_byte_access));
									op(Action::PerformOperation, seq("np"));
									op(int(Action::AssembleWordAddressFromPrefetch) | MicroOp::DestinationMask, seq("nw np", { ea(1) }, !is_byte_access));
								continue;

								case bw2(XXXl, XXXw):		// MOVE.bw (xxx).l, (xxx).w
									op(int(Action::None), seq("np"));
									op(int(Action::AssembleLongWordAddressFromPrefetch) | MicroOp::SourceMask, seq("np nr", { ea(0) }, !is_byte_access));
									op(Action::PerformOperation);
									op(int(Action::AssembleWordAddressFromPrefetch) | MicroOp::DestinationMask, seq("np nw np", { ea(1) }, !is_byte_access));
								break;

								case bw2(XXXw, XXXl):		// MOVE.bw (xxx).w, (xxx).L
									op(int(Action::AssembleWordAddressFromPrefetch) | MicroOp::SourceMask, seq("np nr", { ea(0) }, !is_byte_access));
									op(Action::PerformOperation, seq("np"));
									op(int(Action::AssembleLongWordAddressFromPrefetch) | MicroOp::DestinationMask, seq("nw np np", { ea(1) }, !is_byte_access));
								continue;

								case bw2(XXXl, XXXl):		// MOVE.bw (xxx).l, (xxx).l
									op(int(Action::None), seq("np"));
									op(int(Action::AssembleLongWordAddressFromPrefetch) | MicroOp::SourceMask, seq("np nr", { ea(0) }, !is_byte_access));
									op(Action::PerformOperation, seq("np"));
									op(int(Action::AssembleLongWordAddressFromPrefetch) | MicroOp::DestinationMask, seq("nw np np", { ea(1) }, !is_byte_access));
								break;

								case l2(XXXw, XXXw):		// MOVE.l (xxx).w (xxx).w
									op(int(Action::AssembleWordAddressFromPrefetch) | MicroOp::SourceMask, seq("np nR+ nr", { ea(0), ea(0) }));
									op(Action::PerformOperation);
									op(int(Action::AssembleWordAddressFromPrefetch) | MicroOp::DestinationMask, seq("np nW+ nw np", { ea(1), ea(1) }));
								break;

								case l2(XXXl, XXXw):		// MOVE.l (xxx).l, (xxx).w
									op(int(Action::None), seq("np"));
									op(int(Action::AssembleLongWordAddressFromPrefetch) | MicroOp::SourceMask, seq("np nR+ nr", { ea(0), ea(0) }));
									op(Action::PerformOperation);
									op(int(Action::AssembleWordAddressFromPrefetch) | MicroOp::DestinationMask, seq("np nW+ nw np", { ea(1), ea(1) }));
								break;

								case l2(XXXl, XXXl):		// MOVE.l (xxx).l, (xxx).l
									op(int(Action::None), seq("np"));
								case l2(XXXw, XXXl):		// MOVE.l (xxx).w (xxx).l
									op(address_action_for_mode(combined_source_mode) | MicroOp::SourceMask, seq("np nR+ nr", { ea(0), ea(0) }));
									op(Action::PerformOperation, seq("np"));
									op(int(Action::AssembleLongWordAddressFromPrefetch) | MicroOp::DestinationMask, seq("nW+ nw np np", { ea(1), ea(1) }));
								break;

							//
							// Default
							//

								default: continue;
							}

							// If any post-incrementing was involved, do the post increment(s).
							if(ea_mode == PostInc || destination_mode == PostInc) {
								const int ea_inc = inc(ea_register);
								const int destination_inc = inc(destination_register);

								// If there are two increments, and both can be done simultaneously, do that.
								// Otherwise do one or two individually.
								if(ea_mode == destination_mode && ea_inc == destination_inc) {
									op(ea_inc | MicroOp::SourceMask | MicroOp::DestinationMask);
								} else {
									if(ea_mode == PostInc) {
										op(ea_inc | MicroOp::SourceMask);
									}
									if(destination_mode == PostInc) {
										op(destination_inc | MicroOp::DestinationMask);
									}
								}
							}
						} break;

						case Decoder::RESET:
							storage_.instructions[instruction].requires_supervisor = true;
							op(Action::None, seq("nn _ np"));
						break;

						case Decoder::TRAP: {
							// TRAP involves some oddly-sequenced stack writes, so is calculated
							// at runtime; also the same sequence is used for illegal instructions.
							// So the entirety is scheduled at runtime.
							op(Action::PerformOperation);
							op();
						} break;

						case Decoder::TST: {
							storage_.instructions[instruction].set_source(storage_, ea_mode, ea_register);

							const int mode = combined_mode(ea_mode, ea_register);
							switch(is_long_word_access ? l(mode) : bw(mode)) {
								default: continue;

								case bw(Dn):		// TST.bw Dn
								case l(Dn):			// TST.l Dn
									op(Action::PerformOperation, seq("np"));
								break;

								case bw(PreDec):	// TST.bw -(An)
									op(dec(ea_register) | MicroOp::SourceMask, seq("n"));
								case bw(Ind):		// TST.bw (An)
								case bw(PostInc):	// TST.bw (An)+
									op(Action::None, seq("nr", { a(ea_register) }, !is_byte_access));
									op(Action::PerformOperation, seq("np"));
									if(mode == PostInc) {
										op(inc(ea_register) | MicroOp::SourceMask);
									}
								break;

								case l(PreDec):		// TST.l -(An)
									op(int(Action::Decrement4) | MicroOp::SourceMask, seq("n"));
								case l(Ind):		// TST.l (An)
								case l(PostInc):	// TST.l (An)+
									op(int(Action::CopyToEffectiveAddress) | MicroOp::SourceMask, seq("nR+ nr", { ea(0), ea(0) }));
									op(Action::PerformOperation, seq("np"));
									if(mode == PostInc) {
										op(int(Action::Increment4) | MicroOp::SourceMask);
									}
								break;

								case bw(d16An):		// TST.bw (d16, An)
								case bw(d8AnXn):	// TST.bw (d8, An, Xn)
									op(calc_action_for_mode(mode) | MicroOp::SourceMask, seq(pseq("np nr", mode), { ea(0) }, !is_byte_access));
									op(Action::PerformOperation, seq("np"));
								break;

								case l(d16An):		// TST.l (d16, An)
								case l(d8AnXn):		// TST.l (d8, An, Xn)
									op(calc_action_for_mode(mode) | MicroOp::SourceMask, seq(pseq("np nR+ nr", mode), { ea(0), ea(0) }));
									op(Action::PerformOperation, seq("np"));
								break;

								case bw(XXXl):		// TST.bw (xxx).l
									op(Action::None, seq("np"));
								case bw(XXXw):		// TST.bw (xxx).w
									op(address_assemble_for_mode(mode) | MicroOp::SourceMask, seq("np nr", { ea(0) }, !is_byte_access));
									op(Action::PerformOperation, seq("np"));
								break;

								case l(XXXl):		// TST.l (xxx).l
									op(Action::None, seq("np"));
								case l(XXXw):		// TST.l (xxx).w
									op(address_assemble_for_mode(mode) | MicroOp::SourceMask, seq("np nR+ nr", { ea(0), ea(0) }));
									op(Action::PerformOperation, seq("np"));
								break;
							}
						} break;

						default:
							std::cerr << "Unhandled decoder " << int(mapping.decoder) << std::endl;
						continue;
					}

					// Add a terminating micro operation if necessary.
					if(!storage_.all_micro_ops_.back().is_terminal()) {
						storage_.all_micro_ops_.emplace_back();
					}

					// Ensure that steps that weren't meant to look terminal aren't terminal.
					for(auto index = micro_op_start; index < storage_.all_micro_ops_.size() - 1; ++index) {
						if(storage_.all_micro_ops_[index].is_terminal()) {
							storage_.all_micro_ops_[index].bus_program = seq("");
						}
					}

					// Install the operation and make a note of where micro-ops begin.
					storage_.instructions[instruction].operation = operation;
					micro_op_pointers[instruction] = micro_op_start;

					// Don't search further through the list of possibilities, unless this is a debugging build,
					// in which case verify there are no double mappings.
#ifndef NDEBUG
					++hits;
					assert(hits == 1);
#else
					break;
#endif
				}
			}

#undef inc
#undef dec
		}

#undef Dn
#undef An
#undef Ind
#undef PostDec
#undef PreDec
#undef d16An
#undef d8AnXn
#undef XXXw
#undef XXXl
#undef d16PC
#undef d8PCXn
#undef Imm

#undef bw
#undef l
#undef source_dest

#undef ea
#undef a
#undef seq
#undef op
#undef pseq

		// Finalise micro-op and program pointers.
		for(size_t instruction = 0; instruction < 65536; ++instruction) {
			if(micro_op_pointers[instruction] != std::numeric_limits<size_t>::max()) {
				storage_.instructions[instruction].micro_operations = &storage_.all_micro_ops_[micro_op_pointers[instruction]];

				auto operation = storage_.instructions[instruction].micro_operations;
				while(!operation->is_terminal()) {
					const auto offset = size_t(operation->bus_program - &arbitrary_base);
					assert(offset >= 0 &&  offset < storage_.all_bus_steps_.size());
					operation->bus_program = &storage_.all_bus_steps_[offset];
					++operation;
				}
			}
		}
	}

	private:
		ProcessorStorage &storage_;
};

}
}

CPU::MC68000::ProcessorStorage::ProcessorStorage() {
	ProcessorStorageConstructor constructor(*this);

	// Create the special programs.
	const size_t reset_offset = constructor.assemble_program("n n n n n nn nF nf nV nv np np");

	const size_t branch_taken_offset = constructor.assemble_program("n np np");
	const size_t branch_byte_not_taken_offset = constructor.assemble_program("nn np");
	const size_t branch_word_not_taken_offset = constructor.assemble_program("nn np np");
	const size_t bsr_offset = constructor.assemble_program("np np");

	const size_t dbcc_condition_true_offset = constructor.assemble_program("nn np np");
	const size_t dbcc_condition_false_no_branch_offset = constructor.assemble_program("n nr np np", { &dbcc_false_address_ });
	const size_t dbcc_condition_false_branch_offset = constructor.assemble_program("n np np");
	// That nr in dbcc_condition_false_no_branch_offset is to look like an np from the wrong address.

	// The reads steps needs to be 32 long-word reads plus an overflow word; the writes just the long words.
	// Addresses and data sources/targets will be filled in at runtime, so anything will do here.
	std::string movem_reads_pattern, movem_writes_pattern;
	std::vector<uint32_t *> addresses;
	for(auto c = 0; c < 64; ++c) {
		movem_reads_pattern += "nr ";
		movem_writes_pattern += "nw ";
		addresses.push_back(nullptr);
	}
	movem_reads_pattern += "nr";
	addresses.push_back(nullptr);
	const size_t movem_read_offset = constructor.assemble_program(movem_reads_pattern, addresses);
	const size_t movem_write_offset = constructor.assemble_program(movem_writes_pattern, addresses);

	// Target addresses and values will be filled in by TRAP/illegal too.
	const size_t trap_offset = constructor.assemble_program("nn nw nw nW nV nv np np", { &precomputed_addresses_[0], &precomputed_addresses_[1], &precomputed_addresses_[2] });

	// Install operations.
	constructor.install_instructions();

	// Realise the special programs as direct pointers.
	reset_bus_steps_ = &all_bus_steps_[reset_offset];

	branch_taken_bus_steps_ = &all_bus_steps_[branch_taken_offset];
	branch_byte_not_taken_bus_steps_ = &all_bus_steps_[branch_byte_not_taken_offset];
	branch_word_not_taken_bus_steps_ = &all_bus_steps_[branch_word_not_taken_offset];
	bsr_bus_steps_ = &all_bus_steps_[bsr_offset];

	dbcc_condition_true_steps_ = &all_bus_steps_[dbcc_condition_true_offset];
	dbcc_condition_false_no_branch_steps_ = &all_bus_steps_[dbcc_condition_false_no_branch_offset];
	dbcc_condition_false_no_branch_steps_[1].microcycle.operation |= Microcycle::IsProgram;
	dbcc_condition_false_no_branch_steps_[2].microcycle.operation |= Microcycle::IsProgram;
	dbcc_condition_false_branch_steps_ = &all_bus_steps_[dbcc_condition_false_branch_offset];

	movem_read_steps_ = &all_bus_steps_[movem_read_offset];
	movem_write_steps_ = &all_bus_steps_[movem_write_offset];

	// Link the trap steps but also fill in the program counter as the source
	// for its parts, and use the computed addresses.
	//
	// Order of output is: PC.l, SR, PC.h.
	trap_steps_ = &all_bus_steps_[trap_offset];
	trap_steps_[1].microcycle.value = trap_steps_[2].microcycle.value = &program_counter_.halves.low;
	trap_steps_[3].microcycle.value = trap_steps_[4].microcycle.value = &destination_bus_data_[0].halves.low;
	trap_steps_[5].microcycle.value = trap_steps_[6].microcycle.value = &program_counter_.halves.high;

	// Also relink the RTE and RTR bus steps to collect the program counter.
	//
	// Assumed order of input: PC.h, SR, PC.l (i.e. the opposite of TRAP's output).
	for(const int instruction: { 0x4e73, 0x4e77 }) {
		auto steps = instructions[instruction].micro_operations[0].bus_program;
		steps[0].microcycle.value = steps[1].microcycle.value = &program_counter_.halves.high;
		steps[4].microcycle.value = steps[5].microcycle.value = &program_counter_.halves.low;
	}

	// Set initial state. Largely TODO.
	active_step_ = reset_bus_steps_;
	effective_address_[0] = 0;
	is_supervisor_ = 1;
	interrupt_level_ = 7;
	address_[7] = 0x00030000;
}

void CPU::MC68000::ProcessorStorage::write_back_stack_pointer() {
	stack_pointers_[is_supervisor_] = address_[7];
}

void CPU::MC68000::ProcessorStorage::set_is_supervisor(bool is_supervisor) {
	const int new_is_supervisor = is_supervisor ? 1 : 0;
	if(new_is_supervisor != is_supervisor_) {
		stack_pointers_[is_supervisor_] = address_[7];
		is_supervisor_ = new_is_supervisor;
		address_[7] = stack_pointers_[is_supervisor_];
	}
}
