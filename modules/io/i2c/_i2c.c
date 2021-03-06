/*
 * Copyright (c) 2019  Moddable Tech, Inc.
 *
 *   This file is part of the Moddable SDK Runtime.
 *
 *   The Moddable SDK Runtime is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   The Moddable SDK Runtime is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public License
 *   along with the Moddable SDK Runtime.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

/*
	I2C - uing Arduino twi API

	to do:
*/

#include "twi.h"			// i2c

#include "xsmc.h"			// xs bindings for microcontroller
#include "xsesp.h"			// esp platform support
#include "mc.xs.h"			// for xsID_* values

#include "builtinCommon.h"

struct I2CRecord {
	uint32_t					hz;
	uint8_t						sda;
	uint8_t						scl;
	uint8_t						address;		// 7-bit
	struct I2CRecord			*next;
};
typedef struct I2CRecord I2CRecord;
typedef struct I2CRecord *I2C;

static I2C gI2C;
static I2C gI2CActive;

static void i2cActivate(I2C i2c);
static uint8_t usingPins(uint8_t sda, uint8_t scl);

void _xs_i2c_constructor(xsMachine *the)
{
	I2C i2c;
	int sda, scl, hz, address;

	xsmcVars(1);

	xsmcGet(xsVar(0), xsArg(0), xsID_sda);
	sda = xsmcToInteger(xsVar(0));
	if ((sda < 0) || (sda > 16))
		xsRangeError("invalid sda");

	xsmcGet(xsVar(0), xsArg(0), xsID_scl);
	scl = xsmcToInteger(xsVar(0));
	if ((scl < 0) || (scl > 16))
		xsRangeError("invalid scl");

	if (usingPins((uint8_t)sda, (uint8_t)scl))
		;
	else if (!builtinArePinsFree((1 << sda) | (1 << scl)))
		xsRangeError("inUse");

	if (!xsmcHas(xsArg(0), xsID_address))
		xsRangeError("address required");
	xsmcGet(xsVar(0), xsArg(0), xsID_address);
	address = xsmcToInteger(xsVar(0));
	if ((address < 0) || (address > 127))
		xsRangeError("invalid address");

	for (i2c = gI2C; i2c; i2c = i2c->next) {
		if ((i2c->sda == sda) && (i2c->scl == scl) && (i2c->address == address))
			xsRangeError("duplicate address");
	}

	xsmcGet(xsVar(0), xsArg(0), xsID_hz);
	hz = xsmcToInteger(xsVar(0));
	if ((hz < 0) || (hz > 20000000))
		xsRangeError("invalid hz");

	i2c = c_malloc(sizeof(I2CRecord));
	if (!i2c)
		xsRangeError("no memory");

	xsmcSetHostData(xsThis, i2c);
	i2c->scl = (uint8_t)scl;
	i2c->sda = (uint8_t)sda;
	i2c->hz = hz;
	i2c->address = address;

	i2c->next = gI2C;
	gI2C = i2c;

	builtinUsePins((1 << sda) | (1 << scl));
}

void _xs_i2c_destructor(void *data)
{
	I2C i2c = data;
	if (!i2c)
		return;

	if (i2c == gI2CActive)
		gI2CActive = NULL;

	if (gI2C == i2c)
		gI2C = i2c->next;
	else {
		I2C walker;
		for (walker = gI2C; walker; walker = walker->next) {
			if (walker->next == i2c) {
				walker->next = i2c->next;
				break;
			}
		}
	}

	if (!usingPins(i2c->sda, i2c->scl))
		builtinFreePins((1 << i2c->sda) | (1 << i2c->scl));

	c_free(i2c);

	if (NULL == gI2C)
		twi_stop();
}

void _xs_i2c_close(xsMachine *the)
{
	I2C i2c = xsmcGetHostData(xsThis);
	if (!i2c) return;
	_xs_i2c_destructor(i2c);
	xsmcSetHostData(xsThis, NULL);
}

void _xs_i2c_read(xsMachine *the)
{
	I2C i2c = xsmcGetHostData(xsThis);
	int length, type;
	int err;
	uint8_t stop = true;

	if ((xsmcArgc > 1) && !xsmcTest(xsArg(1)))
		stop = false;

	type = xsmcTypeOf(xsArg(0));
	if ((xsIntegerType == type) || (xsNumberType == type)) {
 		length = xsmcToInteger(xsArg(0));
		xsmcSetArrayBuffer(xsResult, NULL, length);
		xsArg(0) = xsResult;
	}
	else {
		xsResult = xsArg(0);
		if (xsmcIsInstanceOf(xsResult, xsTypedArrayPrototype))
			xsmcGet(xsArg(0), xsResult, xsID_buffer);
		length = xsmcGetArrayBufferLength(xsArg(0));
	}

	i2cActivate(i2c);
	err = twi_readFrom(i2c->address, xsmcToArrayBuffer(xsArg(0)), length, stop);
	if (err)
		xsUnknownError("i2c read failed");
}

void _xs_i2c_write(xsMachine *the)
{
	I2C i2c = xsmcGetHostData(xsThis);
	int err, length;
	uint8_t stop = true;

	if ((xsmcArgc > 1) && !xsmcTest(xsArg(1)))
		stop = false;

	if (xsmcIsInstanceOf(xsArg(0), xsTypedArrayPrototype))
		xsmcGet(xsArg(0), xsArg(0), xsID_buffer);
	length = xsmcGetArrayBufferLength(xsArg(0));

	i2cActivate(i2c);
	err = twi_writeTo(i2c->address, xsmcToArrayBuffer(xsArg(0)), length, stop);
	if (err)
		xsUnknownError("i2c write failed");
}

void i2cActivate(I2C i2c)
{
	if ((i2c == gI2CActive) ||
		(gI2CActive && (gI2CActive->sda == i2c->sda) && (gI2CActive->scl == i2c->scl) && (gI2CActive->hz == i2c->hz)))
		return;

	twi_init(i2c->sda, i2c->scl);
	twi_setClock(i2c->hz);

	gI2CActive = i2c;
}

uint8_t usingPins(uint8_t sda, uint8_t scl)
{
	I2C walker;

	for (walker = gI2C; walker; walker = walker->next) {
		if ((walker->sda == sda) && (walker->scl == scl))
			return 1;
	}

	return 0;
}
