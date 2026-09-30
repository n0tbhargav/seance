import signal, sys, time, os
def size():
    try: return os.get_terminal_size()
    except OSError: return None
def prompt(*a):
    sys.stdout.write("\r\x1b[KPROMPT(%s)> " % (size(),)); sys.stdout.flush()
signal.signal(signal.SIGWINCH, prompt)
sys.stdout.write("first line\r\n"); prompt()
while True: time.sleep(0.2)
