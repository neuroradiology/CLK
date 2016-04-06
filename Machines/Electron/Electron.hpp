//
//  Electron.hpp
//  Clock Signal
//
//  Created by Thomas Harte on 03/01/2016.
//  Copyright © 2016 Thomas Harte. All rights reserved.
//

#ifndef Electron_hpp
#define Electron_hpp

#include "../../Processors/6502/CPU6502.hpp"
#include "../../Outputs/CRT/CRT.hpp"
#include "../../Outputs/Speaker.hpp"
#include "../../Storage/Tape/Tape.hpp"
#include <stdint.h>

namespace Electron {

enum ROMSlot: uint8_t {
	ROMSlot0 = 0,
	ROMSlot1,	ROMSlot2,	ROMSlot3,
	ROMSlot4,	ROMSlot5,	ROMSlot6,	ROMSlot7,

	ROMSlotKeyboard = 8,	ROMSlot9,
	ROMSlotBASIC = 10,		ROMSlot11,

	ROMSlot12,	ROMSlot13,	ROMSlot14,	ROMSlot15,

	ROMSlotOS
};

enum Interrupt: uint8_t {
	PowerOnReset		= 0x02,
	DisplayEnd			= 0x04,
	RealTimeClock		= 0x08,
	ReceiveDataFull		= 0x10,
	TransmitDataEmpty	= 0x20,
	HighToneDetect		= 0x40
};

enum Key: uint16_t {
	KeySpace		= 0x0000 | 0x08,										KeyCopy			= 0x0000 | 0x02,	KeyRight		= 0x0000 | 0x01,
	KeyDelete		= 0x0010 | 0x08,	KeyReturn		= 0x0010 | 0x04,	KeyDown			= 0x0010 | 0x02,	KeyLeft			= 0x0010 | 0x01,
										KeyColon		= 0x0020 | 0x04,	KeyUp			= 0x0020 | 0x02,	KeyMinus		= 0x0020 | 0x01,
	KeySlash		= 0x0030 | 0x08,	KeySemiColon	= 0x0030 | 0x04,	KeyP			= 0x0030 | 0x02,	Key0			= 0x0030 | 0x01,
	KeyFullStop		= 0x0040 | 0x08,	KeyL			= 0x0040 | 0x04,	KeyO			= 0x0040 | 0x02,	Key9			= 0x0040 | 0x01,
	KeyComma		= 0x0050 | 0x08,	KeyK			= 0x0050 | 0x04,	KeyI			= 0x0050 | 0x02,	Key8			= 0x0050 | 0x01,
	KeyM			= 0x0060 | 0x08,	KeyJ			= 0x0060 | 0x04,	KeyU			= 0x0060 | 0x02,	Key7			= 0x0060 | 0x01,
	KeyN			= 0x0070 | 0x08,	KeyH			= 0x0070 | 0x04,	KeyY			= 0x0070 | 0x02,	Key6			= 0x0070 | 0x01,
	KeyB			= 0x0080 | 0x08,	KeyG			= 0x0080 | 0x04,	KeyT			= 0x0080 | 0x02,	Key5			= 0x0080 | 0x01,
	KeyV			= 0x0090 | 0x08,	KeyF			= 0x0090 | 0x04,	KeyR			= 0x0090 | 0x02,	Key4			= 0x0090 | 0x01,
	KeyC			= 0x00a0 | 0x08,	KeyD			= 0x00a0 | 0x04,	KeyE			= 0x00a0 | 0x02,	Key3			= 0x00a0 | 0x01,
	KeyX			= 0x00b0 | 0x08,	KeyS			= 0x00b0 | 0x04,	KeyW			= 0x00b0 | 0x02,	Key2			= 0x00b0 | 0x01,
	KeyZ			= 0x00c0 | 0x08,	KeyA			= 0x00c0 | 0x04,	KeyQ			= 0x00c0 | 0x02,	Key1			= 0x00c0 | 0x01,
	KeyShift		= 0x00d0 | 0x08,	KeyControl		= 0x00d0 | 0x04,	KeyFunc			= 0x00d0 | 0x02,	KeyEscape		= 0x00d0 | 0x01,

	KeyBreak		= 0xffff
};

class Tape {
	public:
		Tape();

		void set_tape(std::shared_ptr<Storage::Tape> tape);

		inline uint8_t get_data_register();
		inline void set_data_register(uint8_t value);
		inline void set_counter(uint8_t value);

		inline uint8_t get_interrupt_status() { return _interrupt_status; }
		inline void clear_interrupts(uint8_t interrupts);

		class Delegate {
			public:
				virtual void tape_did_change_interrupt_status(Tape *tape) = 0;
		};
		inline void set_delegate(Delegate *delegate) { _delegate = delegate; }

		inline void run_for_cycles(unsigned int number_of_cycles);
		inline void run_for_input_pulse();

		inline void set_is_running(bool is_running) { _is_running = is_running; }
		inline void set_is_enabled(bool is_enabled) { _is_enabled = is_enabled; }
		inline void set_is_in_input_mode(bool is_in_input_mode);

	private:
		inline void push_tape_bit(uint16_t bit);
		inline void get_next_tape_pulse();

		std::shared_ptr<Storage::Tape> _tape;

		struct {
			Storage::Tape::Pulse current_pulse;
			std::unique_ptr<SignalProcessing::Stepper> pulse_stepper;
			uint32_t time_into_pulse;
			int minimum_bits_until_full;
		} _input;
		struct {
			unsigned int cycles_into_pulse;
			unsigned int bits_remaining_until_empty;
		} _output;

		bool _is_running;
		bool _is_enabled;
		bool _is_in_input_mode;

		inline void evaluate_interrupts();
		uint16_t _data_register;

		uint8_t _interrupt_status, _last_posted_interrupt_status;
		Delegate *_delegate;

		enum {
			Long, Short, Unrecognised, Recognised
		} _crossings[4];
};

class Speaker: public ::Outputs::Filter<Speaker> {
	public:
		void set_divider(uint8_t divider);

		void set_is_enabled(bool is_enabled);
		inline bool get_is_enabled() { return _is_enabled; }

		void get_samples(unsigned int number_of_samples, int16_t *target);
		void skip_samples(unsigned int number_of_samples);

	private:
		unsigned int _counter;
		uint8_t _divider;
		bool _is_enabled;
};

/*!
	@abstract Represents an Acorn Electron.
	
	@discussion An instance of Electron::Machine represents the current state of an
	Acorn Electron.
*/
class Machine: public CPU6502::Processor<Machine>, Tape::Delegate {

	public:

		Machine();

		unsigned int perform_bus_operation(CPU6502::BusOperation operation, uint16_t address, uint8_t *value);

		void set_rom(ROMSlot slot, size_t length, const uint8_t *data);
		void set_tape(std::shared_ptr<Storage::Tape> tape);

		void set_key_state(Key key, bool isPressed);

		void setup_output();
		Outputs::CRT::CRT *get_crt() { return _crt.get(); }
		Outputs::Speaker *get_speaker() { return &_speaker; }

		virtual void tape_did_change_interrupt_status(Tape *tape);

		void update_output();
		inline void set_use_fast_tape_hack(bool activate) { _use_fast_tape_hack = activate; }

	private:

		inline void update_display();
		inline void start_pixel_line();
		inline void end_pixel_line();
		inline void output_pixels(unsigned int number_of_cycles);

		inline void update_audio();
		inline void signal_interrupt(Interrupt interrupt);
		inline void clear_interrupt(Interrupt interrupt);
		inline void evaluate_interrupts();

		// Things that directly constitute the memory map.
		uint8_t _roms[16][16384];
		uint8_t _os[16384], _ram[32768];

		// Things affected by registers, explicitly or otherwise.
		uint8_t _interrupt_status, _interrupt_control;
		uint8_t _palette[16];
		uint8_t _key_states[14];
		ROMSlot _active_rom;
		uint8_t _screen_mode;
		uint16_t _screenModeBaseAddress;
		uint16_t _startScreenAddress;

		// Counters related to simultaneous subsystems;
		unsigned int _frameCycles, _displayOutputPosition;
		int _audioOutputPosition, _audioOutputPositionError;

		struct {
			uint16_t forty1bpp[256];
			uint8_t forty2bpp[256];
			uint32_t eighty1bpp[256];
			uint16_t eighty2bpp[256];
			uint8_t eighty4bpp[256];
		} _paletteTables;

		// Display generation.
		uint16_t _startLineAddress, _currentScreenAddress;
		int _current_pixel_line, _current_pixel_column, _current_character_row;
		uint8_t _last_pixel_byte;
		bool _isBlankLine;

		// CRT output
		uint8_t *_current_output_target, *_initial_output_target;
		unsigned int _current_output_divider;

		// Tape.
		Tape _tape;
		bool _use_fast_tape_hack;
		bool _fast_load_is_in_data;

		// Outputs.
		std::unique_ptr<Outputs::CRT::CRT> _crt;
		Speaker _speaker;
};

}

#endif /* Electron_hpp */
