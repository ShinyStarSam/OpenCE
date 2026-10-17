"""M0: sends script lines to the game's telnet console and prints what comes
back. The game must run with debug.telnet_console (HALO_TELNET_CONSOLE=1).

  python m0_console.py "line" ["line" ...]

Each line is at most 127 characters, as the console reads them.
"""

import socket
import sys
import time

PORT = 2323


def main():
	lines = sys.argv[1:]
	with socket.create_connection(("127.0.0.1", PORT), timeout=5) as conn:
		conn.settimeout(0.3)
		for line in lines:
			if len(line) > 127:
				sys.exit(f"too long for the console ({len(line)} characters): {line}")
			conn.sendall(line.encode() + b"\r\n")
			reply, deadline = b"", time.time() + 1.5
			while time.time() < deadline:
				try:
					chunk = conn.recv(4096)
				except socket.timeout:
					if reply:
						break
					continue
				if not chunk:
					break
				reply += chunk
			# the console echoes what it is sent; show only what follows the echo
			text = reply.decode("latin-1").replace("\r", "")
			echo = text.find(line)
			text = text[echo + len(line):] if echo >= 0 else text
			print(f"> {line}\n{text.strip()}")


if __name__ == "__main__":
	main()
