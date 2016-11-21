//
//  CommodoreTAP.cpp
//  Clock Signal
//
//  Created by Thomas Harte on 25/06/2016.
//  Copyright © 2016 Thomas Harte. All rights reserved.
//

#include "CommodoreTAP.hpp"
#include <cstdio>
#include <cstring>

using namespace Storage::Tape;

CommodoreTAP::CommodoreTAP(const char *file_name) :
	is_at_end_(false),
	Storage::FileHolder(file_name)
{
	// read and check the file signature
	char signature[12];
	if(fread(signature, 1, 12, file_) != 12)
		throw ErrorNotCommodoreTAP;

	if(memcmp(signature, "C64-TAPE-RAW", 12))
		throw ErrorNotCommodoreTAP;

	// check the file version
	int version = fgetc(file_);
	switch(version)
	{
		case 0:		updated_layout_ = false;	break;
		case 1:		updated_layout_ = true;		break;
		default:	throw ErrorNotCommodoreTAP;
	}

	// skip reserved bytes
	fseek(file_, 3, SEEK_CUR);

	// read file size
	file_size_ = (uint32_t)fgetc(file_);
	file_size_ |= (uint32_t)(fgetc(file_) << 8);
	file_size_ |= (uint32_t)(fgetc(file_) << 16);
	file_size_ |= (uint32_t)(fgetc(file_) << 24);

	// set up for pulse output at the PAL clock rate, with each high and
	// low being half of whatever length values will be read; pretend that
	// a high pulse has just been distributed to imply that the next thing
	// that needs to happen is a length check
	current_pulse_.length.clock_rate = 985248 * 2;
	current_pulse_.type = Pulse::High;
}

void CommodoreTAP::virtual_reset()
{
	fseek(file_, 0x14, SEEK_SET);
	current_pulse_.type = Pulse::High;
	is_at_end_ = false;
}

bool CommodoreTAP::is_at_end()
{
	return is_at_end_;
}

Storage::Tape::Tape::Pulse CommodoreTAP::virtual_get_next_pulse()
{
	if(is_at_end_)
	{
		return current_pulse_;
	}

	if(current_pulse_.type == Pulse::High)
	{
		uint32_t next_length;
		uint8_t next_byte = (uint8_t)fgetc(file_);
		if(!updated_layout_ || next_byte > 0)
		{
			next_length = (uint32_t)next_byte << 3;
		}
		else
		{
			next_length = (uint32_t)fgetc(file_);
			next_length |= (uint32_t)(fgetc(file_) << 8);
			next_length |= (uint32_t)(fgetc(file_) << 16);
		}

		if(feof(file_))
		{
			is_at_end_ = true;
			current_pulse_.length.length = current_pulse_.length.clock_rate;
			current_pulse_.type = Pulse::Zero;
		}
		else
		{
			current_pulse_.length.length = next_length;
			current_pulse_.type = Pulse::Low;
		}
	}
	else
		current_pulse_.type = Pulse::High;

	return current_pulse_;
}
