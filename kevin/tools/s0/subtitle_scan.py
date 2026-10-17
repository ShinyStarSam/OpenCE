"""S0: does a map's dialogue carry subtitle text?

Reads an Xbox Halo cache file (maps/*.map), walks every sound tag ('snd!')
and its permutations, and reports how many have subtitle_data, with
examples. Layouts follow source/cache/cache_files.c and
source/sound/sound_definitions.h; tags are linked to load at 0x803A6000.

  python subtitle_scan.py MAP [MAP...]
"""

import struct
import sys
import zlib

TAG_BASE = 0x803A6000
HEADER_SIZE = 0x800
SOUND_SIZE, PITCH_RANGE_SIZE, PERMUTATION_SIZE = 0xA4, 0x48, 0x7C
SOUND_PITCH_RANGES = 152          # tag_block in sound_definition
PITCH_RANGE_PERMUTATIONS = 60     # tag_block in sound_pitch_range
PERMUTATION_MOUTH, PERMUTATION_SUBTITLE = 84, 104   # tag_data in sound_permutation


def load(path):
	data = open(path, "rb").read()
	signature, version, file_length = struct.unpack_from("<4sii", data, 0)
	tag_data_offset, tag_data_size = struct.unpack_from("<ii", data, 0x10)
	name = data[0x20:0x40].split(b"\0")[0].decode()
	if len(data) < file_length:
		# compressed on disc: the header, then one zlib stream of the rest
		data = data[:HEADER_SIZE] + zlib.decompress(data[HEADER_SIZE:])
	tags = data[tag_data_offset:tag_data_offset + tag_data_size]
	return name, version, data, tags


def main():
	for path in sys.argv[1:]:
		name, version, data, tags = load(path)

		def at(address, size):
			offset = address - TAG_BASE
			if address == 0 or offset < 0 or offset + size > len(tags):
				return None
			return tags[offset:offset + size]

		def block(raw, offset):
			count, address = struct.unpack_from("<iI", raw, offset)
			return count, address

		header = tags[:0x24]
		instances, _, _, tag_count = struct.unpack_from("<Iiii", header, 0)
		table = at(instances, tag_count * 0x20)
		sounds = with_subtitles = with_mouth = permutations = 0
		examples = []
		for i in range(tag_count):
			group, _, _, _, name_address, base = struct.unpack_from("<4s4s4siII", table, i * 0x20)
			if group[::-1] != b"snd!":
				continue
			sounds += 1
			sound = at(base, SOUND_SIZE)
			if sound is None:
				continue
			raw_name = at(name_address, 256) or b""
			tag_name = raw_name.split(b"\0")[0].decode("latin-1")
			pr_count, pr_address = block(sound, SOUND_PITCH_RANGES)
			for p in range(max(pr_count, 0)):
				pitch_range = at(pr_address + p * PITCH_RANGE_SIZE, PITCH_RANGE_SIZE)
				if pitch_range is None:
					continue
				perm_count, perm_address = block(pitch_range, PITCH_RANGE_PERMUTATIONS)
				for q in range(max(perm_count, 0)):
					perm = at(perm_address + q * PERMUTATION_SIZE, PERMUTATION_SIZE)
					if perm is None:
						continue
					permutations += 1
					mouth_size = struct.unpack_from("<i", perm, PERMUTATION_MOUTH)[0]
					sub_size, _, sub_file_offset, sub_address = struct.unpack_from("<iIiI", perm, PERMUTATION_SUBTITLE)
					with_mouth += mouth_size > 0
					if sub_size > 0:
						with_subtitles += 1
						text = at(sub_address, sub_size)
						if text is None and 0 < sub_file_offset < len(data):
							text = data[sub_file_offset:sub_file_offset + sub_size]
						if len(examples) < 8 and text:
							examples.append((tag_name, text[:120]))
		print(f"{path}: map {name!r} version {version}")
		print(f"  sound tags {sounds}, permutations {permutations}, with mouth data {with_mouth}, with subtitle data {with_subtitles}")
		for tag_name, text in examples:
			print(f"    {tag_name}: {text!r}")


if __name__ == "__main__":
	main()
