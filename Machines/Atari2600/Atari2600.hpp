//
//  Atari2600.hpp
//  CLK
//
//  Created by Thomas Harte on 14/07/2015.
//  Copyright © 2015 Thomas Harte. All rights reserved.
//

#ifndef Atari2600_cpp
#define Atari2600_cpp

#include <stdint.h>

#include "../../Processors/6502/CPU6502.hpp"
#include "../CRTMachine.hpp"
#include "PIA.hpp"
#include "Speaker.hpp"
#include "TIA.hpp"

#include "../ConfigurationTarget.hpp"
#include "Atari2600Inputs.h"

namespace Atari2600 {

const unsigned int number_of_upcoming_events = 6;
const unsigned int number_of_recorded_counters = 7;

class Machine:
	public CPU6502::Processor<Machine>,
	public CRTMachine::Machine,
	public ConfigurationTarget::Machine {

	public:
		Machine();
		~Machine();

		void configure_as_target(const StaticAnalyser::Target &target);
		void switch_region();

		void set_digital_input(Atari2600DigitalInput input, bool state);
		void set_switch_is_enabled(Atari2600Switch input, bool state);

		// to satisfy CPU6502::Processor
		unsigned int perform_bus_operation(CPU6502::BusOperation operation, uint16_t address, uint8_t *value);
		void synchronise();

		// to satisfy CRTMachine::Machine
		virtual void setup_output(float aspect_ratio);
		virtual void close_output();
		virtual std::shared_ptr<Outputs::CRT::CRT> get_crt() { return tia_->get_crt(); }
		virtual std::shared_ptr<Outputs::Speaker> get_speaker() { return speaker_; }
		virtual void run_for_cycles(int number_of_cycles) { CPU6502::Processor<Machine>::run_for_cycles(number_of_cycles); }

	private:
		uint8_t *rom_, *rom_pages_[4];
		size_t rom_size_;

		// the RIOT and TIA
		PIA mos6532_;
		std::unique_ptr<TIA> tia_;

		// joystick state
		uint8_t tia_input_value_[2];

		// outputs
		std::shared_ptr<Speaker> speaker_;

		// speaker backlog accumlation counter
		unsigned int cycles_since_speaker_update_;
		void update_audio();

		// video backlog accumulation counter
		unsigned int cycles_since_video_update_;
		void update_video();
};

}

#endif /* Atari2600_cpp */
