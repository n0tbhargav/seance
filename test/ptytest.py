import os, pty, time, struct, fcntl, termios, select, sys
pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"]="xterm-256color"
    os.execvp("bash", ["bash","-il"])
def size(r,c): fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", r, c, 0, 0))
def drain(t=0.7):
    out=b""
    while select.select([fd],[],[],t)[0]:
        try: d=os.read(fd,4096)
        except OSError: break
        if not d: break
        out+=d
    return out
size(30,100); print("initial:", drain(1.5)); sys.stdout.flush()
os.write(fd,b"echo hi\n"); print("cmd:", drain()); sys.stdout.flush()
size(30,50); print("resize1:", drain()); sys.stdout.flush()
size(15,50); print("resize2:", drain()); sys.stdout.flush()
