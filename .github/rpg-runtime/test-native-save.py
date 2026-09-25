#!/usr/bin/env python3
"""Exercise a fresh libretro process reading a Lutro native save file."""
import ctypes
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


def run_core(core: Path, game: Path, saves: Path):
    library = ctypes.CDLL(str(core))
    environment_type = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_uint, ctypes.c_void_p)
    save_path = ctypes.c_char_p(str(saves).encode())

    def environment(command, pointer):
        if command == 31:  # RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY
            ctypes.cast(pointer, ctypes.POINTER(ctypes.c_char_p))[0] = save_path
            return True
        if command == 28:  # RETRO_ENVIRONMENT_GET_PERF_INTERFACE: browser frontend omits this
            return False
        return command == 10  # RETRO_ENVIRONMENT_SET_PIXEL_FORMAT

    callback = environment_type(environment)
    library.retro_set_environment.argtypes = [environment_type]
    library.retro_set_environment(callback)
    library.retro_init()

    class GameInfo(ctypes.Structure):
        _fields_ = [("path", ctypes.c_char_p), ("data", ctypes.c_void_p),
                    ("size", ctypes.c_size_t), ("meta", ctypes.c_char_p)]

    library.retro_load_game.argtypes = [ctypes.POINTER(GameInfo)]
    library.retro_load_game.restype = ctypes.c_bool
    info = GameInfo(str(game).encode(), None, 0, None)
    if not library.retro_load_game(ctypes.byref(info)):
        raise RuntimeError("Lutro failed to load its own save fixture")
    library.retro_unload_game()
    library.retro_deinit()


def main():
    core = Path(__file__).resolve().parents[2] / "lutro_libretro.so"
    if len(sys.argv) == 4 and sys.argv[1] == "--child":
        run_core(core, Path(sys.argv[2]), Path(sys.argv[3]))
        return
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        game = root / "SaveFixture.lutro"
        saves = root / "saves"
        saves.mkdir()
        script = '''
function lutro.load()
  local expected = string.char(0, 255, 65)
  local saved = lutro.filesystem.read("status")
  if saved == nil then
    assert(lutro.filesystem.write("status", expected))
  else
    assert(saved == expected)
    assert(lutro.filesystem.exists("status"))
    assert(lutro.filesystem.isFile("status"))
    local found = false
    for _, item in ipairs(lutro.filesystem.getDirectoryItems("")) do
      if item == "status" then found = true end
    end
    assert(found)
    assert(lutro.filesystem.write("resume-proof", saved))
  end
end
'''
        with zipfile.ZipFile(game, "w") as archive:
            archive.writestr("main.lua", script)
        for _ in range(2):
            subprocess.run([sys.executable, __file__, "--child", str(game), str(saves)], check=True)
        native = saves / "lutro-native" / "SaveFixture"
        assert (native / "status").read_bytes() == bytes([0, 255, 65])
        assert (native / "resume-proof").read_bytes() == bytes([0, 255, 65])
        assert not (saves / "lutro" / "SaveFixture").exists(), "temporary extraction survived unload"


if __name__ == "__main__":
    main()
