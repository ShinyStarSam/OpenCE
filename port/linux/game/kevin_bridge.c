/*
KEVIN_BRIDGE.C

The bridge between the game and Kevin's sidecar (kevin.enabled, kevin.port;
kevin/BRIDGE_PLAN.md, milestone M1). The sidecar is the program that is
Kevin's mind: speech, reflexes, brain. It connects to 127.0.0.1 on
kevin.port, one client at a time.

Every message, both ways, is framed: a 4-byte length (little endian, of
what follows), a 1-byte kind (1: JSON), then the payload. Each tick, before
the AI's update, the bridge reads what the sidecar sent, so a command takes
effect on this tick's control, and sends:

- `state`, every third tick (10 a second): you, Kevin, the units near you;
- `events`, in batches every 2.5 seconds or when a batch is full; an
  event of a type the sidecar marked urgent goes at once. Each says whether
  Kevin was in range of it, saw it (in his view, with a line of sight) or
  did it;
- `applied` for each command on the tick it took effect, and
  `action_result` for each command refused;
- while the sidecar asks to hear you (`hearing`), `ptt` as the push to
  talk key (controls.push_to_talk) goes down and up, and between, what the
  microphone hears (kind 2: 16 kHz), from a little before the press;
- `voice` as Kevin's voice starts and stops playing.

Kevin's voice comes as kind 3 messages (48 kHz), queued and played
through voice chat's mixer (voice_audio.c); a new line cuts off the last.

The sidecar's commands lay an overlay on the AI's control of Kevin
(`press`, `hold`, `fire_mode`, `aim_at`); the AI keeps running beneath,
and takes over again as the overlay ends. `release` clears it at once, as
the bridge does when the sidecar goes, or sends nothing for 5 seconds:
Kevin is then the game's AI alone.

What the sidecar sends is untrusted: a message's size, its JSON, every
field's type and range are checked, and a message that fails is refused
with a reason, never applied in part.
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "bungie_net/network/transport.h"
#include "bungie_net/network/transport_address_constants.h"
#include "bungie_net/network/transport_endpoint.h"
#include "game/game.h"
#include "game/players.h"
#include "items/weapons.h"
#include "math/real_math.h"
#include "objects/objects.h"
#include "physics/collisions.h"
#include "scenario/scenario.h"
#include "tag_files/tag_files.h"
#include "units/unit_control_data.h"
#include "units/units.h"

#include "kevin_bot.h"
#include "kevin_bridge.h"
#include "kevin_view.h"
#include "../src/voice_audio.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* port_config.c's */
int config_boolean(const char *name);
long config_integer(const char *name);

unsigned long system_milliseconds(void);
/* xinput_sdl.c's; console.c's */
int halo_push_to_talk_held(void);
boolean console_is_active(void);

/* ---------- constants */

enum
{
	KEVIN_BRIDGE_PROTOCOL = 1,
	KEVIN_BRIDGE_DEFAULT_PORT = 2324,
	/* the transport's TCP type (as the telnet console's) */
	_kevin_transport_type_tcp = 0x12,

	_kevin_message_json = 1,
	/* what the microphone hears: the utterance, the frame's sequence in
	it (both 4 bytes, little endian), then 16-bit samples at 16 kHz */
	_kevin_message_hearing = 2,
	/* Kevin's voice: his line (4 bytes), then 16-bit samples at 48 kHz */
	_kevin_message_voice = 3,

	/* a message's header: its length and its kind */
	KEVIN_HEADER_SIZE = 5,
	/* the longest message the sidecar may send */
	KEVIN_MAXIMUM_INCOMING = 16 * 1024,
	KEVIN_RECEIVE_BUFFER_SIZE = 2 * (KEVIN_HEADER_SIZE + KEVIN_MAXIMUM_INCOMING),
	/* what waits to be sent; a sidecar this far behind isn't reading */
	KEVIN_SEND_BUFFER_SIZE = 512 * 1024,
	KEVIN_MAXIMUM_OUTGOING = 64 * 1024,

	/* ticks between `state` messages */
	KEVIN_STATE_TICKS = 3,
	/* events are sent this often (ticks), or when this many wait */
	KEVIN_EVENT_TICKS = 75,
	KEVIN_EVENT_BATCH = 16,
	KEVIN_EVENT_BUFFER_SIZE = 24 * 1024,
	/* milliseconds without a message before Kevin is handed back to the AI */
	KEVIN_SILENT_MILLISECONDS = 5000,

	KEVIN_MAXIMUM_URGENT_TYPES = 32,
	KEVIN_MAXIMUM_TYPE_LENGTH = 32,
	KEVIN_MAXIMUM_PENDING = 8,
	KEVIN_MAXIMUM_NEARBY_UNITS = 12,

	/* hearing: a frame of 20 ms at 16 kHz (a third of the microphone's
	48 kHz), and the frames kept from before the press */
	KEVIN_HEARING_FRAME_SAMPLES = VOICE_FRAME_SAMPLES / 3,
	KEVIN_HEARING_PREROLL_FRAMES = 15,
	/* Kevin's voice: his number to the mixer (machines' are below 16), the
	samples queued at most (10 seconds), and those kept in the mixer */
	KEVIN_VOICE_SPEAKER = 1000,
	KEVIN_VOICE_QUEUE_SAMPLES = 10 * 48000,
	KEVIN_VOICE_MIXER_SAMPLES = 48000 / 5,

	/* the longest press or aim (ticks) */
	KEVIN_MAXIMUM_PRESS_TICKS = 300,
	KEVIN_MAXIMUM_AIM_TICKS = 900,
	KEVIN_DEFAULT_PRESS_TICKS = 3,
	KEVIN_DEFAULT_AIM_TICKS = 60,

	/* JSON: the most tokens, and nesting, of one message */
	KEVIN_JSON_MAXIMUM_TOKENS = 128,
	KEVIN_JSON_MAXIMUM_DEPTH = 8,

	_kevin_json_object = 0,
	_kevin_json_array,
	_kevin_json_string,
	_kevin_json_primitive,

	_kevin_aim_none = 0,
	_kevin_aim_you,
	_kevin_aim_object,
	_kevin_aim_point,
};

/* units near you that `state` lists, no further than this (world units) */
#define KEVIN_NEARBY_DISTANCE 30.f
/* Kevin is in range of an event this near him, and sees it within this of
his aim (a cosine: 60 degrees) */
#define KEVIN_WITNESS_DISTANCE 40.f
#define KEVIN_WITNESS_COSINE 0.5f

/* ---------- structures */

struct kevin_json_token
{
	short type;
	short parent;
	long start;
	long end;
};

struct kevin_json
{
	char const *text;
	long length;
	long position;
	struct kevin_json_token tokens[KEVIN_JSON_MAXIMUM_TOKENS];
	long count;
};

struct kevin_control_flag_name
{
	char const *name;
	short bit;
};

/* ---------- globals */

static struct kevin_control_flag_name const kevin_control_flag_names[] =
{
	{ "crouch", _unit_control_crouch_modifier_bit },
	{ "jump", _unit_control_jump_bit },
	{ "flashlight", _unit_control_integrated_light_bit },
	{ "action", _unit_control_action_bit },
	{ "reload", _unit_control_weapon_reload_bit },
	{ "fire", _unit_control_weapon_primary_trigger_bit },
	{ "fire_secondary", _unit_control_weapon_secondary_trigger_bit },
	{ "grenade", _unit_control_throw_grenade_bit },
	{ "swap_weapons", _unit_control_swap_weapons_bit },
};

static struct
{
	boolean initialized;
	struct transport_endpoint *listening_endpoint;
	struct transport_endpoint *client;
	unsigned long last_message_milliseconds;
	boolean silent;               /* released for silence, until the next message */

	byte receive[KEVIN_RECEIVE_BUFFER_SIZE];
	long received;
	byte send[KEVIN_SEND_BUFFER_SIZE];
	long sending;                 /* bytes waiting in send */

	long generation;
	long state_ticks;

	/* events: a JSON array's members, waiting to be sent */
	char events[KEVIN_EVENT_BUFFER_SIZE];
	long events_length;
	long event_count;
	long event_ticks;
	long event_serial;
	char urgent[KEVIN_MAXIMUM_URGENT_TYPES][KEVIN_MAXIMUM_TYPE_LENGTH];
	long urgent_count;

	/* the overlay */
	short press_ticks[NUMBER_OF_UNIT_CONTROL_FLAGS];
	unsigned long hold_flags;
	boolean hold_fire;
	short aim;
	long aim_object_index;
	real_point3d aim_point;
	long aim_ticks;

	/* commands waiting for the tick they take effect */
	long pending_ids[KEVIN_MAXIMUM_PENDING];
	long pending_count;

	/* hearing you */
	boolean hearing;
	boolean microphone_open;
	boolean talking;
	long utterance;
	long hearing_sequence;
	short preroll[KEVIN_HEARING_PREROLL_FRAMES][KEVIN_HEARING_FRAME_SAMPLES];
	long preroll_count;
	long preroll_next;

	/* Kevin's voice */
	short voice[KEVIN_VOICE_QUEUE_SAMPLES];
	long voice_read;
	long voice_count;
	long voice_line;
	boolean voice_playing;
	boolean voice_overflowed;

	/* what is watched for events */
	long kevin_unit_index;
	real kevin_body;
	real kevin_shield;
	long your_unit_index;
	boolean you_dead;
} kevin_bridge_globals;

/* ---------- private code: JSON in */

static void kevin_json_skip_space(
	struct kevin_json *json)
{
	while (json->position < json->length)
	{
		char c = json->text[json->position];

		if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
			break;
		json->position++;
	}

	return;
}

static long kevin_json_token_new(
	struct kevin_json *json,
	short type,
	short parent)
{
	struct kevin_json_token *token;

	if (json->count >= KEVIN_JSON_MAXIMUM_TOKENS)
		return NONE;
	token = &json->tokens[json->count];
	token->type = type;
	token->parent = parent;
	token->start = json->position;
	token->end = json->position;

	return json->count++;
}

/* a string's token spans what is between its quotes */
static long kevin_json_string(
	struct kevin_json *json,
	short parent)
{
	long token_index;

	json->position++;
	token_index = kevin_json_token_new(json, _kevin_json_string, parent);
	if (token_index == NONE)
		return NONE;
	while (json->position < json->length)
	{
		unsigned char c = (unsigned char)json->text[json->position];

		if (c == '"')
		{
			json->tokens[token_index].end = json->position;
			json->position++;
			return token_index;
		}
		if (c < 0x20)
			return NONE;
		if (c == '\\')
		{
			json->position++;
			if (json->position >= json->length)
				return NONE;
			if (json->text[json->position] == 'u')
				json->position += 4;
		}
		json->position++;
	}

	return NONE;
}

static long kevin_json_value(
	struct kevin_json *json,
	short parent,
	long depth);

static long kevin_json_container(
	struct kevin_json *json,
	short parent,
	long depth,
	boolean object)
{
	long token_index;
	char close = object ? '}' : ']';

	if (depth >= KEVIN_JSON_MAXIMUM_DEPTH)
		return NONE;
	token_index = kevin_json_token_new(json, object ? _kevin_json_object : _kevin_json_array, parent);
	if (token_index == NONE)
		return NONE;
	json->position++;
	kevin_json_skip_space(json);
	if (json->position < json->length && json->text[json->position] == close)
	{
		json->position++;
		json->tokens[token_index].end = json->position;
		return token_index;
	}
	while (json->position < json->length)
	{
		long member_parent = token_index;

		if (object)
		{
			long key_index;

			if (json->text[json->position] != '"')
				return NONE;
			key_index = kevin_json_string(json, (short)token_index);
			if (key_index == NONE)
				return NONE;
			kevin_json_skip_space(json);
			if (json->position >= json->length || json->text[json->position] != ':')
				return NONE;
			json->position++;
			member_parent = key_index;
		}
		if (kevin_json_value(json, (short)member_parent, depth + 1) == NONE)
			return NONE;
		kevin_json_skip_space(json);
		if (json->position >= json->length)
			return NONE;
		if (json->text[json->position] == close)
		{
			json->position++;
			json->tokens[token_index].end = json->position;
			return token_index;
		}
		if (json->text[json->position] != ',')
			return NONE;
		json->position++;
		kevin_json_skip_space(json);
	}

	return NONE;
}

static long kevin_json_value(
	struct kevin_json *json,
	short parent,
	long depth)
{
	long token_index;
	char c;

	kevin_json_skip_space(json);
	if (json->position >= json->length)
		return NONE;
	c = json->text[json->position];
	if (c == '{' || c == '[')
		return kevin_json_container(json, parent, depth, c == '{');
	if (c == '"')
		return kevin_json_string(json, parent);

	/* a number, true, false or null */
	token_index = kevin_json_token_new(json, _kevin_json_primitive, parent);
	if (token_index == NONE)
		return NONE;
	while (json->position < json->length)
	{
		c = json->text[json->position];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '-' || c == '+' || c == '.' || c == 'E'))
			break;
		json->position++;
	}
	json->tokens[token_index].end = json->position;
	if (json->tokens[token_index].end == json->tokens[token_index].start)
		return NONE;

	return token_index;
}

/* the whole text one object; FALSE when it isn't */
static boolean kevin_json_parse(
	struct kevin_json *json,
	char const *text,
	long length)
{
	json->text = text;
	json->length = length;
	json->position = 0;
	json->count = 0;
	kevin_json_skip_space(json);
	if (json->position >= length || text[json->position] != '{')
		return FALSE;
	if (kevin_json_value(json, NONE, 0) != 0)
		return FALSE;
	kevin_json_skip_space(json);

	return json->position == length;
}

static boolean kevin_json_token_equals(
	struct kevin_json const *json,
	long token_index,
	char const *string)
{
	struct kevin_json_token const *token = &json->tokens[token_index];
	long length = token->end - token->start;

	return (long)strlen(string) == length && memcmp(json->text + token->start, string, (size_t)length) == 0;
}

/* the value of an object's member, or NONE */
static long kevin_json_find(
	struct kevin_json const *json,
	long object_index,
	char const *key)
{
	long token_index;

	if (object_index == NONE || json->tokens[object_index].type != _kevin_json_object)
		return NONE;
	for (token_index = object_index + 1; token_index < json->count; token_index++)
	{
		if (json->tokens[token_index].parent == object_index &&
			kevin_json_token_equals(json, token_index, key) &&
			token_index + 1 < json->count &&
			json->tokens[token_index + 1].parent == token_index)
		{
			return token_index + 1;
		}
	}

	return NONE;
}

/* a string's text as it is (escapes kept), cut to the buffer; FALSE when
the value isn't a string */
static boolean kevin_json_get_string(
	struct kevin_json const *json,
	long token_index,
	char *buffer,
	long size)
{
	struct kevin_json_token const *token;
	long length;

	if (token_index == NONE || json->tokens[token_index].type != _kevin_json_string || size <= 0)
		return FALSE;
	token = &json->tokens[token_index];
	length = MIN(token->end - token->start, size - 1);
	memcpy(buffer, json->text + token->start, (size_t)length);
	buffer[length] = 0;

	return TRUE;
}

/* a number: digits, a fraction and an exponent; FALSE when it isn't one */
static boolean kevin_json_get_number(
	struct kevin_json const *json,
	long token_index,
	double *value)
{
	struct kevin_json_token const *token;
	char const *c;
	char const *end;
	double sign = 1.0;
	double result = 0.0;
	double scale = 1.0;
	long exponent = 0;
	long exponent_sign = 1;
	boolean digits = FALSE;

	if (token_index == NONE || json->tokens[token_index].type != _kevin_json_primitive)
		return FALSE;
	token = &json->tokens[token_index];
	c = json->text + token->start;
	end = json->text + token->end;
	if (c < end && *c == '-')
	{
		sign = -1.0;
		c++;
	}
	while (c < end && *c >= '0' && *c <= '9')
	{
		result = result * 10.0 + (*c++ - '0');
		digits = TRUE;
	}
	if (c < end && *c == '.')
	{
		c++;
		while (c < end && *c >= '0' && *c <= '9')
		{
			scale *= 0.1;
			result += (*c++ - '0') * scale;
			digits = TRUE;
		}
	}
	if (c < end && (*c == 'e' || *c == 'E'))
	{
		c++;
		if (c < end && (*c == '-' || *c == '+'))
			exponent_sign = *c++ == '-' ? -1 : 1;
		while (c < end && *c >= '0' && *c <= '9' && exponent < 400)
			exponent = exponent * 10 + (*c++ - '0');
		while (exponent-- > 0)
			result = exponent_sign > 0 ? result * 10.0 : result * 0.1;
	}
	if (!digits || c != end || result > 1e30)
		return FALSE;
	*value = sign * result;

	return TRUE;
}

static boolean kevin_json_get_boolean(
	struct kevin_json const *json,
	long token_index,
	boolean *value)
{
	if (token_index == NONE || json->tokens[token_index].type != _kevin_json_primitive)
		return FALSE;
	if (kevin_json_token_equals(json, token_index, "true"))
		*value = TRUE;
	else if (kevin_json_token_equals(json, token_index, "false"))
		*value = FALSE;
	else
		return FALSE;

	return TRUE;
}

/* ---------- private code: sending */

static void kevin_bridge_disconnect(
	char const *why);

static void kevin_bridge_flush(
	void)
{
	long written = 0;

	while (kevin_bridge_globals.client && written < kevin_bridge_globals.sending)
	{
		long result = write_endpoint(
			kevin_bridge_globals.client,
			kevin_bridge_globals.send + written,
			kevin_bridge_globals.sending - written);

		if (result > 0)
		{
			written += result;
		}
		else if (result == _transport_result_operation_would_block)
		{
			break;
		}
		else
		{
			kevin_bridge_disconnect(transport_error_to_string((short)result));
			return;
		}
	}
	if (written > 0)
	{
		memmove(kevin_bridge_globals.send, kevin_bridge_globals.send + written,
			(size_t)(kevin_bridge_globals.sending - written));
		kevin_bridge_globals.sending -= written;
	}

	return;
}

/* queues one message of a kind; a sidecar so far behind that it doesn't
fit is not reading, and is let go */
static void kevin_bridge_send(
	byte kind,
	void const *payload,
	long length)
{
	byte *header;

	if (!kevin_bridge_globals.client || length <= 0 || length > KEVIN_MAXIMUM_OUTGOING)
		return;
	if (kevin_bridge_globals.sending + KEVIN_HEADER_SIZE + length > KEVIN_SEND_BUFFER_SIZE)
	{
		kevin_bridge_disconnect("not reading");
		return;
	}
	header = kevin_bridge_globals.send + kevin_bridge_globals.sending;
	header[0] = (byte)((length + 1) & 0xFF);
	header[1] = (byte)(((length + 1) >> 8) & 0xFF);
	header[2] = (byte)(((length + 1) >> 16) & 0xFF);
	header[3] = (byte)(((length + 1) >> 24) & 0xFF);
	header[4] = kind;
	memcpy(header + KEVIN_HEADER_SIZE, payload, (size_t)length);
	kevin_bridge_globals.sending += KEVIN_HEADER_SIZE + length;

	return;
}

static void kevin_bridge_send_json(
	char const *json,
	long length)
{
	kevin_bridge_send(_kevin_message_json, json, length);

	return;
}

/* appends to a message being written; FALSE once it's full */
static boolean kevin_bridge_append(
	char *buffer,
	long size,
	long *length,
	char const *format,
	...)
{
	va_list arguments;
	int written;

	if (*length >= size - 1)
		return FALSE;
	va_start(arguments, format);
	written = vsnprintf(buffer + *length, (size_t)(size - *length), format, arguments);
	va_end(arguments);
	if (written < 0 || *length + written >= size)
	{
		*length = size - 1;
		return FALSE;
	}
	*length += written;

	return TRUE;
}

/* a string for JSON: quotes, backslashes and control characters escaped */
static void kevin_bridge_append_string(
	char *buffer,
	long size,
	long *length,
	char const *string)
{
	kevin_bridge_append(buffer, size, length, "\"");
	while (string && *string)
	{
		unsigned char c = (unsigned char)*string++;

		if (c == '"' || c == '\\')
			kevin_bridge_append(buffer, size, length, "\\%c", c);
		else if (c < 0x20)
			kevin_bridge_append(buffer, size, length, "\\u%04x", c);
		else
			kevin_bridge_append(buffer, size, length, "%c", c);
	}
	kevin_bridge_append(buffer, size, length, "\"");

	return;
}

/* an object id as JSON: its datum index, unsigned, or null for none */
static void kevin_bridge_append_object(
	char *buffer,
	long size,
	long *length,
	char const *name,
	long object_index)
{
	if (object_index == NONE)
		kevin_bridge_append(buffer, size, length, "\"%s\":null", name);
	else
		kevin_bridge_append(buffer, size, length, "\"%s\":%lu", name, (unsigned long)object_index);

	return;
}

static void kevin_bridge_send_result(
	long id,
	boolean ok,
	char const *reason)
{
	char message[256];
	long length = 0;

	kevin_bridge_append(message, sizeof(message), &length,
		"{\"type\":\"action_result\",\"id\":%ld,\"ok\":%s,\"tick\":%ld,\"reason\":",
		id, ok ? "true" : "false", game_time_get());
	kevin_bridge_append_string(message, sizeof(message), &length, reason);
	kevin_bridge_append(message, sizeof(message), &length, "}");
	kevin_bridge_send_json(message, length);

	return;
}

static char const *kevin_bridge_tag_name(
	long object_index)
{
	struct object_datum *object = object_index != NONE ? object_try_and_get(object_index) : NULL;

	return object ? tag_name_strip_path(tag_get_name(object->definition_index)) : "";
}

/* ---------- private code: the overlay */

static void kevin_bridge_release(
	char const *why)
{
	boolean any = kevin_bridge_globals.hold_flags || kevin_bridge_globals.hold_fire ||
		kevin_bridge_globals.aim != _kevin_aim_none;
	short flag;

	for (flag = 0; flag < NUMBER_OF_UNIT_CONTROL_FLAGS; flag++)
	{
		if (kevin_bridge_globals.press_ticks[flag])
			any = TRUE;
		kevin_bridge_globals.press_ticks[flag] = 0;
	}
	kevin_bridge_globals.hold_flags = 0;
	kevin_bridge_globals.hold_fire = FALSE;
	kevin_bridge_globals.aim = _kevin_aim_none;
	kevin_bridge_globals.aim_ticks = 0;
	kevin_bridge_globals.pending_count = 0;
	if (any)
		error(_error_silent, "kevin bridge: Kevin is the game's AI again (%s)", why);

	return;
}

static void kevin_bridge_pending_add(
	long id)
{
	if (kevin_bridge_globals.pending_count < KEVIN_MAXIMUM_PENDING)
		kevin_bridge_globals.pending_ids[kevin_bridge_globals.pending_count++] = id;

	return;
}

static short kevin_bridge_flag_by_name(
	char const *name)
{
	long index;

	for (index = 0; index < (long)NUMBEROF(kevin_control_flag_names); index++)
	{
		if (!strcmp(kevin_control_flag_names[index].name, name))
			return kevin_control_flag_names[index].bit;
	}

	return NONE;
}

/* a count of ticks from a member, within its limits, or the default */
static boolean kevin_bridge_ticks(
	struct kevin_json const *json,
	char const *key,
	long maximum,
	long default_ticks,
	long *ticks)
{
	long token_index = kevin_json_find(json, 0, key);
	double value;

	*ticks = default_ticks;
	if (token_index == NONE)
		return TRUE;
	if (!kevin_json_get_number(json, token_index, &value) || value < 1.0 || value > (double)maximum)
		return FALSE;
	*ticks = (long)value;

	return TRUE;
}

/* ---------- private code: Kevin's voice */

static void kevin_bridge_voice_stop(
	void)
{
	kevin_bridge_globals.voice_read = 0;
	kevin_bridge_globals.voice_count = 0;
	voice_audio_forget(KEVIN_VOICE_SPEAKER);

	return;
}

/* a kind 3 message: samples of a line queued; a new line cuts off the last */
static void kevin_bridge_voice_message(
	byte const *payload,
	long length)
{
	long line;
	long count;
	long index;

	if (length < 4 || ((length - 4) & 1))
	{
		kevin_bridge_send_result(NONE, FALSE, "voice is a line number and 16-bit samples");
		return;
	}
	line = (long)((unsigned long)payload[0] | ((unsigned long)payload[1] << 8) |
		((unsigned long)payload[2] << 16) | ((unsigned long)payload[3] << 24));
	if (line != kevin_bridge_globals.voice_line)
	{
		kevin_bridge_voice_stop();
		kevin_bridge_globals.voice_line = line;
		kevin_bridge_globals.voice_overflowed = FALSE;
	}
	count = (length - 4) / 2;
	for (index = 0; index < count; index++)
	{
		byte const *sample = payload + 4 + 2 * index;

		if (kevin_bridge_globals.voice_count >= KEVIN_VOICE_QUEUE_SAMPLES)
		{
			if (!kevin_bridge_globals.voice_overflowed)
				error(_error_silent, "kevin bridge: Kevin's line %ld is longer than his queue", line);
			kevin_bridge_globals.voice_overflowed = TRUE;
			break;
		}
		kevin_bridge_globals.voice[(kevin_bridge_globals.voice_read + kevin_bridge_globals.voice_count) %
			KEVIN_VOICE_QUEUE_SAMPLES] = (short)((unsigned short)sample[0] | ((unsigned short)sample[1] << 8));
		kevin_bridge_globals.voice_count++;
	}

	return;
}

/* the queue into the mixer, a little ahead of it; `voice` as he starts
and stops */
static void kevin_bridge_speak(
	void)
{
	boolean playing;

	while (kevin_bridge_globals.voice_count > 0 &&
		voice_audio_buffered(KEVIN_VOICE_SPEAKER) < KEVIN_VOICE_MIXER_SAMPLES)
	{
		float samples[VOICE_FRAME_SAMPLES];
		long count = MIN(kevin_bridge_globals.voice_count, VOICE_FRAME_SAMPLES);
		long index;

		for (index = 0; index < count; index++)
		{
			samples[index] = (float)kevin_bridge_globals.voice[kevin_bridge_globals.voice_read] / 32768.f;
			kevin_bridge_globals.voice_read = (kevin_bridge_globals.voice_read + 1) % KEVIN_VOICE_QUEUE_SAMPLES;
		}
		kevin_bridge_globals.voice_count -= count;
		voice_audio_play_samples(KEVIN_VOICE_SPEAKER, samples, (int)count, 1.f, 0.f);
	}
	playing = kevin_bridge_globals.voice_count > 0 || voice_audio_speaking(KEVIN_VOICE_SPEAKER);
	if (playing != kevin_bridge_globals.voice_playing)
	{
		char message[128];
		long length = 0;

		kevin_bridge_globals.voice_playing = playing;
		kevin_bridge_append(message, sizeof(message), &length,
			"{\"type\":\"voice\",\"line\":%ld,\"playing\":%s,\"tick\":%ld,\"ms\":%lu}",
			kevin_bridge_globals.voice_line, playing ? "true" : "false", game_time_get(), system_milliseconds());
		kevin_bridge_send_json(message, length);
	}

	return;
}

/* ---------- private code: hearing you */

static void kevin_bridge_send_ptt(
	boolean down)
{
	char message[128];
	long length = 0;

	kevin_bridge_append(message, sizeof(message), &length,
		"{\"type\":\"ptt\",\"down\":%s,\"utterance\":%ld,\"tick\":%ld,\"ms\":%lu}",
		down ? "true" : "false", kevin_bridge_globals.utterance, game_time_get(), system_milliseconds());
	kevin_bridge_send_json(message, length);

	return;
}

static void kevin_bridge_send_hearing(
	short const *samples)
{
	byte message[8 + 2 * KEVIN_HEARING_FRAME_SAMPLES];
	unsigned long utterance = (unsigned long)kevin_bridge_globals.utterance;
	unsigned long sequence = (unsigned long)kevin_bridge_globals.hearing_sequence++;
	long index;

	for (index = 0; index < 4; index++)
	{
		message[index] = (byte)((utterance >> (8 * index)) & 0xFF);
		message[4 + index] = (byte)((sequence >> (8 * index)) & 0xFF);
	}
	for (index = 0; index < KEVIN_HEARING_FRAME_SAMPLES; index++)
	{
		message[8 + 2 * index] = (byte)((unsigned short)samples[index] & 0xFF);
		message[9 + 2 * index] = (byte)(((unsigned short)samples[index] >> 8) & 0xFF);
	}
	kevin_bridge_send(_kevin_message_hearing, message, sizeof(message));

	return;
}

/* the microphone open while the sidecar listens; what it hears sent while
the push to talk key is held, from the frames kept before the press */
static void kevin_bridge_hear(
	void)
{
	float frame[VOICE_FRAME_SAMPLES];
	boolean held;

	if (!kevin_bridge_globals.client || !kevin_bridge_globals.hearing)
	{
		if (kevin_bridge_globals.microphone_open)
			voice_audio_microphone(FALSE);
		kevin_bridge_globals.microphone_open = FALSE;
		if (kevin_bridge_globals.talking)
			kevin_bridge_send_ptt(FALSE);
		kevin_bridge_globals.talking = FALSE;
		kevin_bridge_globals.preroll_count = 0;
		return;
	}
	voice_audio_microphone(TRUE);
	kevin_bridge_globals.microphone_open = TRUE;
	held = halo_push_to_talk_held() && !console_is_active();
	if (held && !kevin_bridge_globals.talking)
	{
		long index;

		kevin_bridge_globals.talking = TRUE;
		kevin_bridge_globals.utterance++;
		kevin_bridge_globals.hearing_sequence = 0;
		kevin_bridge_send_ptt(TRUE);
		for (index = 0; index < kevin_bridge_globals.preroll_count; index++)
		{
			long oldest = (kevin_bridge_globals.preroll_next - kevin_bridge_globals.preroll_count + index +
				KEVIN_HEARING_PREROLL_FRAMES) % KEVIN_HEARING_PREROLL_FRAMES;

			kevin_bridge_send_hearing(kevin_bridge_globals.preroll[oldest]);
		}
		kevin_bridge_globals.preroll_count = 0;
	}
	while (voice_audio_read_frame(frame))
	{
		short samples[KEVIN_HEARING_FRAME_SAMPLES];
		long index;

		/* (48 kHz to 16, three samples averaged) */
		for (index = 0; index < KEVIN_HEARING_FRAME_SAMPLES; index++)
		{
			real sample = (frame[3 * index] + frame[3 * index + 1] + frame[3 * index + 2]) * (32767.f / 3.f);

			samples[index] = (short)PIN(sample, -32768.f, 32767.f);
		}
		if (kevin_bridge_globals.talking)
		{
			kevin_bridge_send_hearing(samples);
		}
		else
		{
			csmemcpy(kevin_bridge_globals.preroll[kevin_bridge_globals.preroll_next], samples, sizeof(samples));
			kevin_bridge_globals.preroll_next = (kevin_bridge_globals.preroll_next + 1) % KEVIN_HEARING_PREROLL_FRAMES;
			if (kevin_bridge_globals.preroll_count < KEVIN_HEARING_PREROLL_FRAMES)
				kevin_bridge_globals.preroll_count++;
		}
	}
	if (!held && kevin_bridge_globals.talking)
	{
		kevin_bridge_globals.talking = FALSE;
		kevin_bridge_send_ptt(FALSE);
	}

	return;
}

/* ---------- private code: commands */

static void kevin_bridge_command(
	char const *text,
	long length)
{
	struct kevin_json json;
	char type[KEVIN_MAXIMUM_TYPE_LENGTH];
	double number;
	long id = NONE;
	long token_index;
	long kevin_unit_index = kevin_bot_unit_index();

	if (!kevin_json_parse(&json, text, length))
	{
		kevin_bridge_send_result(NONE, FALSE, "not a JSON object");
		return;
	}
	token_index = kevin_json_find(&json, 0, "id");
	if (token_index != NONE)
	{
		if (!kevin_json_get_number(&json, token_index, &number) || number < 0.0 || number > 2147483647.0)
		{
			kevin_bridge_send_result(NONE, FALSE, "id is not a whole number");
			return;
		}
		id = (long)number;
	}
	if (!kevin_json_get_string(&json, kevin_json_find(&json, 0, "type"), type, sizeof(type)))
	{
		kevin_bridge_send_result(id, FALSE, "no type");
		return;
	}
	token_index = kevin_json_find(&json, 0, "generation");
	if (token_index != NONE &&
		(!kevin_json_get_number(&json, token_index, &number) || (long)number != kevin_bridge_globals.generation))
	{
		kevin_bridge_send_result(id, FALSE, "stale generation");
		return;
	}

	if (!strcmp(type, "ping"))
	{
		char message[160];
		long message_length = 0;

		token_index = kevin_json_find(&json, 0, "t");
		if (!kevin_json_get_number(&json, token_index, &number))
			number = 0.0;
		kevin_bridge_append(message, sizeof(message), &message_length,
			"{\"type\":\"pong\",\"id\":%ld,\"t\":%.3f,\"tick\":%ld}", id, number, game_time_get());
		kevin_bridge_send_json(message, message_length);
		return;
	}
	if (!strcmp(type, "release"))
	{
		kevin_bridge_release("the sidecar asked");
		kevin_bridge_send_result(id, TRUE, "released");
		return;
	}
	if (!strcmp(type, "urgent"))
	{
		long count = 0;
		long array_index = kevin_json_find(&json, 0, "types");

		if (array_index == NONE || json.tokens[array_index].type != _kevin_json_array)
		{
			kevin_bridge_send_result(id, FALSE, "types is not an array");
			return;
		}
		for (token_index = array_index + 1; token_index < json.count; token_index++)
		{
			if (json.tokens[token_index].parent != array_index)
				continue;
			if (count >= KEVIN_MAXIMUM_URGENT_TYPES ||
				!kevin_json_get_string(&json, token_index, kevin_bridge_globals.urgent[count], KEVIN_MAXIMUM_TYPE_LENGTH))
			{
				kevin_bridge_send_result(id, FALSE, "too many types, or one isn't a string");
				return;
			}
			count++;
		}
		kevin_bridge_globals.urgent_count = count;
		kevin_bridge_send_result(id, TRUE, "urgent types set");
		return;
	}
	if (!strcmp(type, "hearing"))
	{
		boolean on = TRUE;

		token_index = kevin_json_find(&json, 0, "on");
		if (token_index != NONE && !kevin_json_get_boolean(&json, token_index, &on))
		{
			kevin_bridge_send_result(id, FALSE, "on is not true or false");
			return;
		}
		kevin_bridge_globals.hearing = on;
		kevin_bridge_send_result(id, TRUE, on ? "hearing" : "not hearing");
		return;
	}
	if (!strcmp(type, "voice_stop"))
	{
		kevin_bridge_voice_stop();
		kevin_bridge_send_result(id, TRUE, "voice stopped");
		return;
	}
	if (!strcmp(type, "request_view"))
	{
		kevin_view_request();
		kevin_bridge_send_result(id, TRUE, "view asked for");
		return;
	}

	/* the overlay's commands need Kevin */
	if (kevin_unit_index == NONE || unit_get(kevin_unit_index)->unit.actor_index == NONE)
	{
		kevin_bridge_send_result(id, FALSE, "no Kevin");
		return;
	}
	if (!strcmp(type, "press") || !strcmp(type, "hold"))
	{
		char name[KEVIN_MAXIMUM_TYPE_LENGTH];
		short flag;

		if (!kevin_json_get_string(&json, kevin_json_find(&json, 0, "flag"), name, sizeof(name)) ||
			(flag = kevin_bridge_flag_by_name(name)) == NONE)
		{
			kevin_bridge_send_result(id, FALSE, "unknown flag");
			return;
		}
		if (!strcmp(type, "press"))
		{
			long ticks;

			if (!kevin_bridge_ticks(&json, "ticks", KEVIN_MAXIMUM_PRESS_TICKS, KEVIN_DEFAULT_PRESS_TICKS, &ticks))
			{
				kevin_bridge_send_result(id, FALSE, "ticks out of range");
				return;
			}
			kevin_bridge_globals.press_ticks[flag] = (short)ticks;
		}
		else
		{
			boolean on = TRUE;

			token_index = kevin_json_find(&json, 0, "on");
			if (token_index != NONE && !kevin_json_get_boolean(&json, token_index, &on))
			{
				kevin_bridge_send_result(id, FALSE, "on is not true or false");
				return;
			}
			if (on)
				kevin_bridge_globals.hold_flags |= FLAG(flag);
			else
				kevin_bridge_globals.hold_flags &= ~FLAG(flag);
		}
		kevin_bridge_pending_add(id);
		return;
	}
	if (!strcmp(type, "fire_mode"))
	{
		char mode[KEVIN_MAXIMUM_TYPE_LENGTH];

		if (!kevin_json_get_string(&json, kevin_json_find(&json, 0, "mode"), mode, sizeof(mode)) ||
			(strcmp(mode, "free") && strcmp(mode, "hold")))
		{
			kevin_bridge_send_result(id, FALSE, "mode is not free or hold");
			return;
		}
		kevin_bridge_globals.hold_fire = !strcmp(mode, "hold");
		kevin_bridge_pending_add(id);
		return;
	}
	if (!strcmp(type, "aim_at"))
	{
		char name[KEVIN_MAXIMUM_TYPE_LENGTH];
		long ticks;
		long target_index = kevin_json_find(&json, 0, "target");

		if (!kevin_bridge_ticks(&json, "ticks", KEVIN_MAXIMUM_AIM_TICKS, KEVIN_DEFAULT_AIM_TICKS, &ticks))
		{
			kevin_bridge_send_result(id, FALSE, "ticks out of range");
			return;
		}
		if (kevin_json_get_string(&json, target_index, name, sizeof(name)) && !strcmp(name, "you"))
		{
			kevin_bridge_globals.aim = _kevin_aim_you;
		}
		else if (kevin_json_get_number(&json, target_index, &number))
		{
			long object_index = (long)(unsigned long)number;

			if (number < 0.0 || number > 4294967295.0 || !object_try_and_get(object_index))
			{
				kevin_bridge_send_result(id, FALSE, "no such object");
				return;
			}
			kevin_bridge_globals.aim = _kevin_aim_object;
			kevin_bridge_globals.aim_object_index = object_index;
		}
		else if (target_index != NONE && json.tokens[target_index].type == _kevin_json_object)
		{
			double x, y, z;

			if (!kevin_json_get_number(&json, kevin_json_find(&json, target_index, "x"), &x) ||
				!kevin_json_get_number(&json, kevin_json_find(&json, target_index, "y"), &y) ||
				!kevin_json_get_number(&json, kevin_json_find(&json, target_index, "z"), &z) ||
				x < -10000.0 || x > 10000.0 || y < -10000.0 || y > 10000.0 || z < -10000.0 || z > 10000.0)
			{
				kevin_bridge_send_result(id, FALSE, "target point out of the world");
				return;
			}
			kevin_bridge_globals.aim = _kevin_aim_point;
			kevin_bridge_globals.aim_point.x = (real)x;
			kevin_bridge_globals.aim_point.y = (real)y;
			kevin_bridge_globals.aim_point.z = (real)z;
		}
		else
		{
			kevin_bridge_send_result(id, FALSE, "target is not you, an object id or a point");
			return;
		}
		kevin_bridge_globals.aim_ticks = ticks;
		kevin_bridge_pending_add(id);
		return;
	}

	kevin_bridge_send_result(id, FALSE, "unknown type");

	return;
}

/* ---------- private code: the connection */

static void kevin_bridge_disconnect(
	char const *why)
{
	struct transport_endpoint *client = kevin_bridge_globals.client;

	kevin_bridge_globals.client = NULL;
	kevin_bridge_globals.received = 0;
	kevin_bridge_globals.sending = 0;
	if (client)
	{
		delete_transport_endpoint(client);
		error(_error_silent, "kevin bridge: the sidecar is gone (%s)", why);
	}
	kevin_bridge_release("the sidecar is gone");
	kevin_bridge_globals.hearing = FALSE;
	kevin_bridge_voice_stop();

	return;
}

static void kevin_bridge_send_hello(
	void)
{
	char message[512];
	long length = 0;

	kevin_bridge_append(message, sizeof(message), &length,
		"{\"type\":\"hello\",\"protocol\":%d,\"game\":\"OpenCE\",\"tick\":%ld,\"generation\":%ld,",
		KEVIN_BRIDGE_PROTOCOL, game_time_get(), kevin_bridge_globals.generation);
	kevin_bridge_append_object(message, sizeof(message), &length, "kevin", kevin_bot_unit_index());
	kevin_bridge_append(message, sizeof(message), &length, ",\"level\":");
	kevin_bridge_append_string(message, sizeof(message), &length,
		global_scenario_index != NONE ? tag_name_strip_path(tag_get_name(global_scenario_index)) : "");
	kevin_bridge_append(message, sizeof(message), &length, "}");
	kevin_bridge_send_json(message, length);

	return;
}

static void kevin_bridge_initialize(
	void)
{
	struct transport_address address = {{0}};
	long port;

	kevin_bridge_globals.initialized = TRUE;
	kevin_bridge_globals.aim_object_index = NONE;
	kevin_bridge_globals.kevin_unit_index = NONE;
	kevin_bridge_globals.your_unit_index = NONE;
	if (!config_boolean("kevin.enabled"))
		return;
	port = config_integer("kevin.port");
	if (port <= 0)
		return;
	if (port > 65535)
		port = KEVIN_BRIDGE_DEFAULT_PORT;

	/* only from this machine: the sidecar's commands drive Kevin */
	kevin_bridge_globals.listening_endpoint = create_transport_endpoint(_kevin_transport_type_tcp);
	if (!kevin_bridge_globals.listening_endpoint)
	{
		error(_error_silent, "kevin bridge: no endpoint");
		return;
	}
	address.address_length = IPV4_ADDRESS_LENGTH;
	address.address.long_words[0] = IPV4_LOOPBACK_ADDRESS;
	address.port = (word)port;
	if (bind_endpoint(kevin_bridge_globals.listening_endpoint, &address) != _transport_error_none ||
		listen_endpoint(kevin_bridge_globals.listening_endpoint) != _transport_error_none)
	{
		error(_error_silent, "kevin bridge: can't listen on 127.0.0.1:%ld", port);
		delete_transport_endpoint(kevin_bridge_globals.listening_endpoint);
		kevin_bridge_globals.listening_endpoint = NULL;
		return;
	}
	error(_error_silent, "kevin bridge: listening on 127.0.0.1:%ld", port);

	return;
}

static void kevin_bridge_accept(
	void)
{
	struct transport_endpoint *endpoint;

	if (!endpoint_readable(kevin_bridge_globals.listening_endpoint, 0))
		return;
	endpoint = accept_endpoint(kevin_bridge_globals.listening_endpoint);
	if (!endpoint)
		return;
	/* (a sidecar that stops reading doesn't stall the game) */
	if (set_endpoint_blocking(endpoint, FALSE) != _transport_error_none)
	{
		delete_transport_endpoint(endpoint);
		return;
	}
	if (kevin_bridge_globals.client)
	{
		delete_transport_endpoint(endpoint);
		error(_error_silent, "kevin bridge: a second sidecar refused");
		return;
	}
	kevin_bridge_globals.client = endpoint;
	kevin_bridge_globals.received = 0;
	kevin_bridge_globals.sending = 0;
	kevin_bridge_globals.last_message_milliseconds = system_milliseconds();
	kevin_bridge_globals.silent = FALSE;
	error(_error_silent, "kevin bridge: the sidecar is connected");
	kevin_bridge_send_hello();

	return;
}

/* reads and runs every whole message waiting */
static void kevin_bridge_receive(
	void)
{
	long offset = 0;

	while (kevin_bridge_globals.client && endpoint_readable(kevin_bridge_globals.client, 0) &&
		kevin_bridge_globals.received < KEVIN_RECEIVE_BUFFER_SIZE)
	{
		long count = read_endpoint(
			kevin_bridge_globals.client,
			kevin_bridge_globals.receive + kevin_bridge_globals.received,
			KEVIN_RECEIVE_BUFFER_SIZE - kevin_bridge_globals.received);

		if (count > 0)
		{
			kevin_bridge_globals.received += count;
		}
		else if (count == _transport_result_operation_would_block)
		{
			break;
		}
		else
		{
			kevin_bridge_disconnect(count == 0 ? "closed" : transport_error_to_string((short)count));
			return;
		}
	}

	while (kevin_bridge_globals.client && kevin_bridge_globals.received - offset >= KEVIN_HEADER_SIZE)
	{
		byte const *header = kevin_bridge_globals.receive + offset;
		unsigned long length = (unsigned long)header[0] | ((unsigned long)header[1] << 8) |
			((unsigned long)header[2] << 16) | ((unsigned long)header[3] << 24);

		if (length < 1 || length > KEVIN_MAXIMUM_INCOMING + 1)
		{
			kevin_bridge_disconnect("a message of a wrong size");
			return;
		}
		if ((unsigned long)(kevin_bridge_globals.received - offset) < 4 + length)
			break;
		kevin_bridge_globals.last_message_milliseconds = system_milliseconds();
		kevin_bridge_globals.silent = FALSE;
		if (header[4] == _kevin_message_json)
			kevin_bridge_command((char const *)header + KEVIN_HEADER_SIZE, (long)length - 1);
		else if (header[4] == _kevin_message_voice)
			kevin_bridge_voice_message(header + KEVIN_HEADER_SIZE, (long)length - 1);
		else
			kevin_bridge_send_result(NONE, FALSE, "unknown message kind");
		offset += 4 + (long)length;
	}
	if (offset > 0 && kevin_bridge_globals.client)
	{
		memmove(kevin_bridge_globals.receive, kevin_bridge_globals.receive + offset,
			(size_t)(kevin_bridge_globals.received - offset));
		kevin_bridge_globals.received -= offset;
	}

	return;
}

/* ---------- private code: state and events */

static long kevin_bridge_your_unit(
	void)
{
	long player_index = local_player_get_player_index(0);

	return player_index != NONE ? player_get(player_index)->unit_index : NONE;
}

/* whether Kevin was in range of a point, and saw it: in front of him, with
nothing in the way */
static void kevin_bridge_witness(
	real_point3d const *point,
	long object_index,
	boolean *in_range,
	boolean *saw)
{
	long kevin_unit_index = kevin_bot_unit_index();
	struct unit_datum *kevin;
	real_point3d eye;
	real_vector3d to_point;
	real_vector3d aim;
	real distance;
	struct collision_result collision;

	*in_range = FALSE;
	*saw = FALSE;
	if (kevin_unit_index == NONE)
		return;
	kevin = unit_get(kevin_unit_index);
	if (TEST_FLAG(kevin->object.damage_flags, _object_dead_bit))
		return;
	unit_get_camera_position(kevin_unit_index, &eye);
	vector_from_points3d(&eye, point, &to_point);
	distance = magnitude3d(&to_point);
	if (distance > KEVIN_WITNESS_DISTANCE)
		return;
	*in_range = TRUE;
	if (distance < 0.5f)
	{
		*saw = TRUE;
		return;
	}
	aim = kevin->unit.aiming_vector;
	if (normalize3d(&aim) == 0.f || dot_product3d(&aim, &to_point) / distance < KEVIN_WITNESS_COSINE)
		return;
	if (!collision_test_vector(_collision_test_for_line_of_sight_flags, &eye, &to_point, kevin_unit_index, &collision))
		*saw = TRUE;
	else
		*saw = object_index != NONE && collision.object_index != NONE &&
			object_get_ultimate_parent(collision.object_index) == object_get_ultimate_parent(object_index);

	return;
}

static void kevin_bridge_flush_events(
	void)
{
	static char message[KEVIN_EVENT_BUFFER_SIZE + 128];
	long length = 0;

	if (!kevin_bridge_globals.event_count)
		return;
	kevin_bridge_append(message, sizeof(message), &length,
		"{\"type\":\"events\",\"tick\":%ld,\"generation\":%ld,\"events\":[%s]}",
		game_time_get(), kevin_bridge_globals.generation, kevin_bridge_globals.events);
	kevin_bridge_send_json(message, length);
	kevin_bridge_globals.events_length = 0;
	kevin_bridge_globals.events[0] = 0;
	kevin_bridge_globals.event_count = 0;
	kevin_bridge_globals.event_ticks = 0;

	return;
}

static boolean kevin_bridge_urgent(
	char const *type)
{
	long index;

	for (index = 0; index < kevin_bridge_globals.urgent_count; index++)
	{
		if (!strcmp(kevin_bridge_globals.urgent[index], type))
			return TRUE;
	}

	return FALSE;
}

static void kevin_bridge_append_point(
	char *buffer,
	long size,
	long *length,
	char const *name,
	real_point3d const *point)
{
	kevin_bridge_append(buffer, size, length, "\"%s\":{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}",
		name, point->x, point->y, point->z);

	return;
}

/* you or Kevin, for `state` */
static void kevin_bridge_append_unit(
	char *message,
	long size,
	long *length,
	char const *name,
	long unit_index)
{
	struct unit_datum *unit;
	real_point3d position;
	short slot;
	long weapon_index;

	if (unit_index == NONE)
	{
		kevin_bridge_append(message, size, length, "\"%s\":null", name);
		return;
	}
	unit = unit_get(unit_index);
	object_get_origin(unit_index, &position);
	slot = unit->unit.current_weapon_index;
	weapon_index = slot >= 0 && slot < MAXIMUM_WEAPONS_PER_UNIT ? unit->unit.weapon_object_indices[slot] : NONE;
	kevin_bridge_append(message, size, length, "\"%s\":{", name);
	kevin_bridge_append_object(message, size, length, "unit", unit_index);
	kevin_bridge_append(message, size, length, ",");
	kevin_bridge_append_point(message, size, length, "position", &position);
	kevin_bridge_append(message, size, length,
		",\"facing\":%.3f,\"aim\":{\"i\":%.3f,\"j\":%.3f,\"k\":%.3f},\"health\":%.3f,\"shield\":%.3f,\"alive\":%s,",
		arctangent(unit->object.forward.j, unit->object.forward.i),
		unit->unit.aiming_vector.i, unit->unit.aiming_vector.j, unit->unit.aiming_vector.k,
		unit->object.body_vitality, unit->object.shield_vitality,
		TEST_FLAG(unit->object.damage_flags, _object_dead_bit) ? "false" : "true");
	kevin_bridge_append_object(message, size, length, "vehicle", unit->object.parent_object_index);
	kevin_bridge_append(message, size, length, ",\"weapon\":");
	kevin_bridge_append_string(message, size, length, kevin_bridge_tag_name(weapon_index));
	kevin_bridge_append(message, size, length, ",\"rounds\":%d}",
		weapon_index != NONE ? (int)weapon_get(weapon_index)->weapon.magazines[0].rounds_loaded : -1);

	return;
}

static void kevin_bridge_send_state(
	void)
{
	static char message[8192];
	long length = 0;
	long your_unit_index = kevin_bridge_your_unit();
	long kevin_unit_index = kevin_bot_unit_index();
	struct object_iterator iterator;
	struct unit_datum *unit;
	real_point3d yours;
	long nearby = 0;

	kevin_bridge_append(message, sizeof(message), &length,
		"{\"type\":\"state\",\"tick\":%ld,\"generation\":%ld,\"overlay\":%s,",
		game_time_get(), kevin_bridge_globals.generation,
		kevin_bridge_globals.hold_flags || kevin_bridge_globals.hold_fire || kevin_bridge_globals.aim ? "true" : "false");
	kevin_bridge_append_unit(message, sizeof(message), &length, "you", your_unit_index);
	kevin_bridge_append(message, sizeof(message), &length, ",");
	kevin_bridge_append_unit(message, sizeof(message), &length, "kevin", kevin_unit_index);
	kevin_bridge_append(message, sizeof(message), &length, ",");
	kevin_bridge_append_object(message, sizeof(message), &length, "crosshair", player_control_get_target_object_index(0));
	kevin_bridge_append(message, sizeof(message), &length, ",\"nearby\":[");

	/* the units near you, but you and him */
	if (your_unit_index != NONE)
	{
		object_get_origin(your_unit_index, &yours);
		object_iterator_new(&iterator, _object_mask_unit, 0);
		while ((unit = object_iterator_next(&iterator)) != NULL && nearby < KEVIN_MAXIMUM_NEARBY_UNITS)
		{
			real_point3d position;

			if (iterator.index == your_unit_index || iterator.index == kevin_unit_index)
				continue;
			object_get_origin(iterator.index, &position);
			if (distance3d(&position, &yours) > KEVIN_NEARBY_DISTANCE)
				continue;
			kevin_bridge_append(message, sizeof(message), &length, "%s{", nearby ? "," : "");
			kevin_bridge_append_object(message, sizeof(message), &length, "id", iterator.index);
			kevin_bridge_append(message, sizeof(message), &length, ",\"tag\":");
			kevin_bridge_append_string(message, sizeof(message), &length, kevin_bridge_tag_name(iterator.index));
			kevin_bridge_append(message, sizeof(message), &length, ",\"team\":%d,\"alive\":%s,",
				(int)unit->object.owner_team_index,
				TEST_FLAG(unit->object.damage_flags, _object_dead_bit) ? "false" : "true");
			kevin_bridge_append_point(message, sizeof(message), &length, "position", &position);
			kevin_bridge_append(message, sizeof(message), &length, "}");
			nearby++;
		}
	}
	kevin_bridge_append(message, sizeof(message), &length, "]}");
	kevin_bridge_send_json(message, length);

	return;
}

/* events found by watching: his damage and who did it, your death */
static void kevin_bridge_watch(
	void)
{
	long kevin_unit_index = kevin_bot_unit_index();
	long your_unit_index = kevin_bridge_your_unit();

	if (kevin_unit_index != kevin_bridge_globals.kevin_unit_index)
	{
		kevin_bridge_globals.kevin_unit_index = kevin_unit_index;
		if (kevin_unit_index != NONE)
		{
			kevin_bridge_globals.kevin_body = unit_get(kevin_unit_index)->object.body_vitality;
			kevin_bridge_globals.kevin_shield = unit_get(kevin_unit_index)->object.shield_vitality;
		}
	}
	else if (kevin_unit_index != NONE)
	{
		struct unit_datum *kevin = unit_get(kevin_unit_index);
		real body = kevin->object.body_vitality;
		real shield = kevin->object.shield_vitality;
		real lost = (kevin_bridge_globals.kevin_body - body) + (kevin_bridge_globals.kevin_shield - shield);

		if (lost > 0.01f && !TEST_FLAG(kevin->object.damage_flags, _object_dead_bit))
		{
			char fields[160];
			long fields_length = 0;
			long attacker_index = NONE;
			long attacker_player_index = NONE;
			unsigned long latest = 0;
			short index;

			for (index = 0; index < MAXIMUM_ATTACKERS_PER_UNIT; index++)
			{
				struct unit_attacker const *attacker = &kevin->unit.attackers[index];

				if (attacker->game_time_stamp != (unsigned long)NONE && attacker->game_time_stamp >= latest)
				{
					latest = attacker->game_time_stamp;
					attacker_index = attacker->object_index;
					attacker_player_index = attacker->player_index;
				}
			}
			kevin_bridge_append(fields, sizeof(fields), &fields_length,
				"\"shield\":%.3f,\"health\":%.3f,\"lost\":%.3f,",
				shield, body, lost);
			kevin_bridge_append_object(fields, sizeof(fields), &fields_length, "attacker", attacker_index);
			if (attacker_player_index != NONE && attacker_player_index == local_player_get_player_index(0))
				kevin_bridge_event("player_shot_kevin", "You shot Kevin.", kevin_unit_index, FALSE, fields);
			else
				kevin_bridge_event("kevin_damaged", "Kevin was hit.", kevin_unit_index, FALSE, fields);
		}
		kevin_bridge_globals.kevin_body = body;
		kevin_bridge_globals.kevin_shield = shield;
	}

	if (your_unit_index != kevin_bridge_globals.your_unit_index)
	{
		kevin_bridge_globals.your_unit_index = your_unit_index;
		kevin_bridge_globals.you_dead = FALSE;
	}
	else if (your_unit_index != NONE)
	{
		boolean dead = TEST_FLAG(unit_get(your_unit_index)->object.damage_flags, _object_dead_bit);

		if (dead && !kevin_bridge_globals.you_dead)
			kevin_bridge_event("player_died", "You died.", your_unit_index, FALSE, NULL);
		kevin_bridge_globals.you_dead = dead;
	}

	return;
}

/* ---------- public code */

void kevin_bridge_update(
	void)
{
	if (!kevin_bridge_globals.initialized)
		kevin_bridge_initialize();
	if (!kevin_bridge_globals.listening_endpoint)
		return;

	kevin_bridge_accept();
	kevin_bridge_receive();
	if (kevin_bridge_globals.client && !kevin_bridge_globals.silent &&
		system_milliseconds() - kevin_bridge_globals.last_message_milliseconds > KEVIN_SILENT_MILLISECONDS)
	{
		kevin_bridge_globals.silent = TRUE;
		kevin_bridge_release("the sidecar is silent");
	}

	kevin_bridge_hear();
	kevin_bridge_speak();
	kevin_bridge_watch();
	if (++kevin_bridge_globals.event_ticks >= KEVIN_EVENT_TICKS)
		kevin_bridge_flush_events();
	if (kevin_bridge_globals.client && ++kevin_bridge_globals.state_ticks >= KEVIN_STATE_TICKS)
	{
		kevin_bridge_globals.state_ticks = 0;
		kevin_bridge_send_state();
	}
	kevin_bridge_flush();

	return;
}

boolean kevin_bridge_look_target(
	long unit_index,
	real_vector3d *target)
{
	real_point3d eye;
	real_point3d point;

	if (kevin_bridge_globals.aim == _kevin_aim_none)
		return FALSE;
	if (kevin_bridge_globals.aim_ticks <= 0)
	{
		kevin_bridge_globals.aim = _kevin_aim_none;
		return FALSE;
	}
	kevin_bridge_globals.aim_ticks--;
	switch (kevin_bridge_globals.aim)
	{
	case _kevin_aim_you:
		if (kevin_bridge_your_unit() == NONE)
			return FALSE;
		unit_get_head_position(kevin_bridge_your_unit(), &point);
		break;
	case _kevin_aim_object:
		if (!object_try_and_get(kevin_bridge_globals.aim_object_index))
		{
			kevin_bridge_globals.aim = _kevin_aim_none;
			return FALSE;
		}
		object_get_origin(kevin_bridge_globals.aim_object_index, &point);
		break;
	default:
		point = kevin_bridge_globals.aim_point;
		break;
	}
	unit_get_camera_position(unit_index, &eye);
	vector_from_points3d(&eye, &point, target);

	return magnitude_squared3d(target) > 0.f;
}

void kevin_bridge_adjust_control(
	long unit_index,
	struct unit_control_data *control_data)
{
	unsigned long flags = 0;
	short flag;
	long index;

	(void)unit_index;
	for (flag = 0; flag < NUMBER_OF_UNIT_CONTROL_FLAGS; flag++)
	{
		if (kevin_bridge_globals.press_ticks[flag] > 0)
		{
			kevin_bridge_globals.press_ticks[flag]--;
			flags |= FLAG(flag);
		}
	}
	flags |= kevin_bridge_globals.hold_flags;
	control_data->control_flags |= (word)flags;
	if (kevin_bridge_globals.hold_fire)
	{
		control_data->control_flags &= (word)~(FLAG(_unit_control_weapon_primary_trigger_bit) |
			FLAG(_unit_control_weapon_secondary_trigger_bit) | FLAG(_unit_control_throw_grenade_bit));
	}

	/* the commands waiting for this tick */
	for (index = 0; index < kevin_bridge_globals.pending_count; index++)
	{
		char message[128];
		long length = 0;

		kevin_bridge_append(message, sizeof(message), &length,
			"{\"type\":\"applied\",\"id\":%ld,\"tick\":%ld}",
			kevin_bridge_globals.pending_ids[index], game_time_get());
		kevin_bridge_send_json(message, length);
	}
	/* (sent now, not with the next tick's messages) */
	if (kevin_bridge_globals.pending_count)
		kevin_bridge_flush();
	kevin_bridge_globals.pending_count = 0;

	return;
}

void kevin_bridge_event(
	char const *type,
	char const *summary,
	long object_index,
	boolean kevin_did,
	char const *fields)
{
	char event[1024];
	long length = 0;
	real_point3d position;
	boolean in_range = FALSE;
	boolean saw = FALSE;
	boolean has_position = object_index != NONE && object_try_and_get(object_index) != NULL;

	if (!kevin_bridge_globals.listening_endpoint)
		return;
	if (has_position)
	{
		object_get_origin(object_index, &position);
		if (object_index == kevin_bot_unit_index())
		{
			in_range = TRUE;
			saw = TRUE;
		}
		else
		{
			kevin_bridge_witness(&position, object_index, &in_range, &saw);
		}
	}
	kevin_bridge_append(event, sizeof(event), &length, "{\"id\":%ld,\"event\":", ++kevin_bridge_globals.event_serial);
	kevin_bridge_append_string(event, sizeof(event), &length, type);
	kevin_bridge_append(event, sizeof(event), &length, ",\"tick\":%ld,\"summary\":", game_time_get());
	kevin_bridge_append_string(event, sizeof(event), &length, summary);
	kevin_bridge_append(event, sizeof(event), &length, ",");
	kevin_bridge_append_object(event, sizeof(event), &length, "object", object_index);
	kevin_bridge_append(event, sizeof(event), &length, ",");
	if (has_position)
	{
		kevin_bridge_append_point(event, sizeof(event), &length, "position", &position);
		kevin_bridge_append(event, sizeof(event), &length, ",");
	}
	kevin_bridge_append(event, sizeof(event), &length,
		"\"kevin_in_range\":%s,\"kevin_saw\":%s,\"kevin_did\":%s",
		in_range || kevin_did ? "true" : "false", saw || kevin_did ? "true" : "false", kevin_did ? "true" : "false");
	if (fields && fields[0])
		kevin_bridge_append(event, sizeof(event), &length, ",%s", fields);
	kevin_bridge_append(event, sizeof(event), &length, "}");
	error(_error_silent, "kevin bridge: event %s", event);

	/* a batch that can't take it goes now */
	if (kevin_bridge_globals.events_length + length + 2 >= KEVIN_EVENT_BUFFER_SIZE)
		kevin_bridge_flush_events();
	if (kevin_bridge_globals.events_length + length + 2 < KEVIN_EVENT_BUFFER_SIZE)
	{
		kevin_bridge_append(kevin_bridge_globals.events, KEVIN_EVENT_BUFFER_SIZE, &kevin_bridge_globals.events_length,
			"%s%s", kevin_bridge_globals.event_count ? "," : "", event);
		kevin_bridge_globals.event_count++;
	}
	if (kevin_bridge_globals.event_count >= KEVIN_EVENT_BATCH || kevin_bridge_urgent(type))
		kevin_bridge_flush_events();

	return;
}

void kevin_bridge_new_generation(
	char const *why)
{
	char fields[96];
	long fields_length = 0;

	kevin_bridge_globals.generation++;
	kevin_bridge_release(why);
	kevin_bridge_append(fields, sizeof(fields), &fields_length, "\"generation\":%ld", kevin_bridge_globals.generation);
	kevin_bridge_event("generation", why, NONE, FALSE, fields);

	return;
}

void kevin_bridge_checkpoint(
	boolean reverted)
{
	char fields[96];
	long fields_length = 0;

	/* (a checkpoint's id is the game time it was saved at, which a revert
	to it brings back) */
	kevin_bridge_append(fields, sizeof(fields), &fields_length, "\"checkpoint\":%ld", game_time_get());
	if (reverted)
	{
		kevin_bridge_event("checkpoint_revert", "The game went back to a checkpoint.", NONE, FALSE, fields);
		kevin_bridge_new_generation("checkpoint revert");
	}
	else
	{
		kevin_bridge_event("checkpoint", "The game saved a checkpoint.", NONE, FALSE, fields);
	}

	return;
}
