#!/usr/bin/env python3
import socket
import sys

# --------- Low-level NGP helpers ---------

def send_message(sock, payload: str, msg_id: int = 1) -> None:
    """
    Send an NGP message with given payload string.

    Wire format: id|len|PAYLOAD
    where len is the number of bytes in PAYLOAD.
    """
    if not payload.endswith("|"):
        payload = payload + "|"
    payload_bytes = payload.encode("utf-8")
    header = f"{msg_id}|{len(payload_bytes)}|".encode("utf-8")
    sock.sendall(header + payload_bytes)


def try_extract_message(buffer: bytes):
    """
    Try to extract a single complete NGP message from buffer.

    Returns:
        (payload_str, remaining_buffer) if a full message is present
        (None, buffer) if we need more data
    """
    # Need at least "x|y|" before payload
    first_bar = buffer.find(b"|")
    if first_bar == -1:
        return None, buffer
    second_bar = buffer.find(b"|", first_bar + 1)
    if second_bar == -1:
        return None, buffer

    length_bytes = buffer[first_bar + 1:second_bar]
    try:
        length_str = length_bytes.decode("utf-8")
        payload_len = int(length_str)
    except Exception:
        raise ValueError("Invalid length field from server")

    total_needed = second_bar + 1 + payload_len
    if len(buffer) < total_needed:
        # Need more bytes
        return None, buffer

    payload_bytes = buffer[second_bar + 1:total_needed]
    try:
        payload = payload_bytes.decode("utf-8")
    except UnicodeDecodeError:
        payload = payload_bytes.decode("utf-8", errors="replace")

    rest = buffer[total_needed:]
    return payload, rest


def iter_messages(sock):
    """
    Generator yielding payload strings from the socket.
    Handles partial/combined TCP reads.
    """
    buffer = b""
    while True:
        # First see if we already have a complete message
        msg, buffer = try_extract_message(buffer)
        if msg is not None:
            yield msg
            continue

        # Otherwise read more data
        data = sock.recv(4096)
        if not data:
            # connection closed
            raise EOFError("Connection closed by server")
        buffer += data


# --------- Protocol-level helpers ---------

def parse_server_payload(payload: str):
    """
    Split server payload "TYPE|...|" -> (TYPE, parts_list)
    """
    if payload.endswith("|"):
        payload = payload[:-1]
    parts = payload.split("|")
    if not parts or parts[0] == "":
        return None, parts
    msg_type = parts[0]
    return msg_type, parts


def pretty_print_board(board):
    print("\nCurrent piles:")
    for i, stones in enumerate(board, start=1):
        print(f"  Pile {i}: {stones} stone(s)")
    print()


def prompt_move(board, player_num):
    """
    Ask user for a valid (pile, quantity) move based on the current board.
    """
    while True:
        try:
            pile_str = input("Choose a pile (1-5), or 'q' to quit: ").strip()
        except EOFError:
            raise SystemExit(0)

        if pile_str.lower() in ("q", "quit", "exit"):
            raise SystemExit(0)

        try:
            pile = int(pile_str)
        except ValueError:
            print("Please enter a number between 1 and 5.")
            continue

        if not (1 <= pile <= 5):
            print("Pile must be between 1 and 5.")
            continue

        if board[pile - 1] <= 0:
            print("That pile is empty. Choose a different pile.")
            continue

        break

    max_take = board[pile - 1]
    while True:
        try:
            qty_str = input(f"How many stones to remove from pile {pile} (1-{max_take}): ").strip()
        except EOFError:
            raise SystemExit(0)

        try:
            qty = int(qty_str)
        except ValueError:
            print("Please enter a valid number.")
            continue

        if not (1 <= qty <= max_take):
            print(f"Quantity must be between 1 and {max_take}.")
            continue

        break

    return pile, qty


# --------- Main client logic ---------

def main():
    # Usage:
    #   python nim_client.py [host] [port] [name]
    if len(sys.argv) >= 3:
        host = sys.argv[1]
        try:
            port = int(sys.argv[2])
        except ValueError:
            print("Port must be an integer.")
            sys.exit(1)

        if len(sys.argv) >= 4:
            name = sys.argv[3]
        else:
            name = input("Enter your player name (max 72 chars): ").strip()
    else:
        host = input("Server host (default: localhost): ").strip() or "localhost"
        port_str = input("Server port: ").strip()
        try:
            port = int(port_str)
        except ValueError:
            print("Port must be an integer.")
            sys.exit(1)
        name = input("Enter your player name (max 72 chars): ").strip()

    if not name:
        print("Name cannot be empty.")
        sys.exit(1)
    if len(name) > 72:
        print("Name too long (max 72 characters).")
        sys.exit(1)

    print(f"\nConnecting to {host}:{port} as '{name}'...\n")

    try:
        sock = socket.create_connection((host, port))
    except OSError as e:
        print(f"Failed to connect: {e}")
        sys.exit(1)

    # Send OPEN handshake
    send_message(sock, f"OPEN|{name}|")
    print("Sent OPEN request. Waiting for server...\n")

    player_num = None
    opponent_name = None
    board = [0, 0, 0, 0, 0]

    try:
        for payload in iter_messages(sock):
            msg_type, parts = parse_server_payload(payload)
            if msg_type is None:
                print(f"Got weird payload from server: {payload!r}")
                continue

            # --- Message handlers ---

            if msg_type == "WAIT":
                print("Server: Waiting for another player to join...")

            elif msg_type == "NAME":
                # NAME|player_num|opponent|p1 p2 p3 p4 p5|
                if len(parts) < 4:
                    print(f"Malformed NAME message: {payload!r}")
                    continue
                try:
                    player_num = int(parts[1])
                except ValueError:
                    print(f"Bad player number in NAME: {parts[1]!r}")
                    continue
                opponent_name = parts[2]
                try:
                    board = [int(x) for x in parts[3].split()]
                except ValueError:
                    print(f"Bad board in NAME: {parts[3]!r}")
                    continue

                print("\n=== Game started! ===")
                print(f"You are Player {player_num}.")
                print(f"Your opponent: {opponent_name}")
                pretty_print_board(board)
                print("On your turn, choose a pile and how many stones to remove.\n")

            elif msg_type == "PLAY":
                # PLAY|whose_turn|p1 p2 p3 p4 p5|
                if len(parts) < 3:
                    print(f"Malformed PLAY message: {payload!r}")
                    continue
                try:
                    whose_turn = int(parts[1])
                except ValueError:
                    print(f"Bad turn number in PLAY: {parts[1]!r}")
                    continue
                try:
                    board = [int(x) for x in parts[2].split()]
                except ValueError:
                    print(f"Bad board in PLAY: {parts[2]!r}")
                    continue

                pretty_print_board(board)

                if player_num is None:
                    print("Got PLAY before NAME? Ignoring.")
                    continue

                if whose_turn == player_num:
                    print(">>> It's YOUR turn!")
                    pile, qty = prompt_move(board, player_num)
                    send_message(sock, f"MOVE|{pile}|{qty}|")
                    print(f"Sent MOVE: pile {pile}, remove {qty}.\n")
                else:
                    print("Waiting for opponent's move...\n")

            elif msg_type == "FAIL":
                # FAIL|code|msg|
                code = None
                reason = ""
                if len(parts) >= 2:
                    try:
                        code = int(parts[1])
                    except ValueError:
                        pass
                if len(parts) >= 3:
                    reason = parts[2]

                print(f"\nServer reported error (FAIL {code}): {reason}")

                # Fatal vs non-fatal
                fatal_codes = {10, 21, 22, 23, 24}
                if code in fatal_codes:
                    print("This error is fatal; closing connection.")
                    break
                else:
                    print("Non-fatal error; you can keep playing.\n")

            elif msg_type == "OVER":
                # OVER|winner|p1 p2 p3 p4 p5|[Forfeit]|
                if len(parts) < 3:
                    print(f"Malformed OVER message: {payload!r}")
                    break
                try:
                    winner = int(parts[1])
                except ValueError:
                    print(f"Bad winner field in OVER: {parts[1]!r}")
                    break
                try:
                    board = [int(x) for x in parts[2].split()]
                except ValueError:
                    print(f"Bad board in OVER: {parts[2]!r}")
                    # still show winner though

                forfeit = False
                if len(parts) >= 4 and parts[3] == "Forfeit":
                    forfeit = True

                pretty_print_board(board)
                if player_num is not None:
                    if winner == player_num:
                        if forfeit:
                            print(">>> You WIN by forfeit! 🎉")
                        else:
                            print(">>> You WIN! 🎉")
                    else:
                        if forfeit:
                            print(">>> You LOSE by forfeit. 😢")
                        else:
                            print(">>> You LOSE. 😢")
                else:
                    print(f"Game over. Winner: Player {winner}")
                break

            elif msg_type == "CONNECTION_FAILED":
                print("Server reported CONNECTION_FAILED. Closing.")
                break

            elif msg_type == "SERVER_SHUTDOWN":
                print("Server is shutting down. Goodbye.")
                break

            else:
                print(f"Unknown message from server: {payload!r}")

    except EOFError:
        print("\nConnection closed by server.")
    except KeyboardInterrupt:
        print("\nInterrupted by user.")
    finally:
        try:
            sock.close()
        except Exception:
            pass


if __name__ == "__main__":
    main()