# test importing of invalid .mpy files

try:
    import sys, io, vfs

    sys.implementation._mpy
    io.IOBase
except (ImportError, AttributeError):
    print("SKIP")
    raise SystemExit


class UserFile(io.IOBase):
    def __init__(self, data):
        self.data = memoryview(data)
        self.pos = 0

    # CIRCUITPY-CHANGE
    def read(self):
        return self.data

    def readinto(self, buf):
        n = min(len(buf), len(self.data) - self.pos)
        buf[:n] = self.data[self.pos : self.pos + n]
        self.pos += n
        return n

    def ioctl(self, req, arg):
        return 0


class UserFS:
    def __init__(self, files):
        self.files = files

    def mount(self, readonly, mksfs):
        pass

    def umount(self):
        pass

    def stat(self, path):
        if path in self.files:
            return (32768, 0, 0, 0, 0, 0, 0, 0, 0, 0)
        raise OSError

    def open(self, path, mode):
        return UserFile(self.files[path])


# these are the test .mpy files
# CIRCUITPY-CHANGE: C instead of M
user_files = {
    "/mod0.mpy": b"",  # empty file
    "/mod1.mpy": b"C",  # too short header
    "/mod2.mpy": b"C\x00\x00\x00",  # bad version
}

# create and mount a user filesystem
vfs.mount(UserFS(user_files), "/userfs")
sys.path.append("/userfs")

# import .mpy files from the user filesystem
for i in range(len(user_files)):
    mod = "mod%u" % i
    try:
        __import__(mod)
    # CIRCUITPY-CHANGE
    except Exception as e:
        print(mod, type(e).__name__, e)

# unmount and undo path addition
vfs.umount("/userfs")
sys.path.pop()
