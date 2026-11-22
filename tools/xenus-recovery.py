#!/usr/bin/env python3
import ctypes
import ctypes.util
import getpass
import os
import pathlib
import secrets
import sys
import tempfile

CONFIG = pathlib.Path.home() / ".config" / "xenus"
AUTH = CONFIG / ".auth"
RECOVERY = CONFIG / ".recovery"


def load_lib():
    name = ctypes.util.find_library("crypt")
    if not name:
        raise RuntimeError("libcrypt tidak ditemukan.")

    lib = ctypes.CDLL(name)

    lib.crypt.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    lib.crypt.restype = ctypes.c_char_p

    lib.crypt_gensalt_rn.argtypes = [
        ctypes.c_char_p, ctypes.c_ulong,
        ctypes.c_void_p, ctypes.c_int,
        ctypes.c_char_p, ctypes.c_int
    ]
    lib.crypt_gensalt_rn.restype = ctypes.c_char_p
    return lib


def make_hash(lib, value):
    salt = ctypes.create_string_buffer(CRYPT_SALT_SIZE)
    result = lib.crypt_gensalt_rn(
        b"$y$", 0, None, 0, salt, len(salt)
    )
    if not result:
        raise RuntimeError("Gagal membuat salt yescrypt.")

    hashed = lib.crypt(value.encode(), salt.value)
    if not hashed or not hashed.startswith(b"$y$"):
        raise RuntimeError("Gagal membuat hash yescrypt.")
    return hashed.decode()


CRYPT_SALT_SIZE = 128


def verify(lib, value, stored):
    result = lib.crypt(value.encode(), stored.encode())
    return bool(result and result.decode() == stored)


def safe_file(path):
    if path.is_symlink() or not path.is_file():
        raise RuntimeError(f"File tidak valid: {path}")
    if path.stat().st_uid != os.getuid():
        raise RuntimeError(f"Kepemilikan file tidak sesuai: {path}")


def atomic_write(path, value):
    fd, temp = tempfile.mkstemp(prefix=".xenus-", dir=CONFIG)
    try:
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(value + "\n")
            f.flush()
            os.fsync(f.fileno())
        os.replace(temp, path)
        os.chmod(path, 0o600)
        dfd = os.open(CONFIG, os.O_RDONLY)
        try:
            os.fsync(dfd)
        finally:
            os.close(dfd)
    finally:
        if os.path.exists(temp):
            os.unlink(temp)


def read_hash(path):
    safe_file(path)
    value = path.read_text().strip()
    if not value.startswith("$y$"):
        raise RuntimeError(f"Format hash tidak dikenal: {path}")
    return value


def password_input(prompt):
    value = getpass.getpass(prompt)
    if len(value) < 8:
        raise RuntimeError("Password minimal 8 karakter.")
    return value


def setup(lib):
    if not AUTH.exists():
        raise RuntimeError("File password XENUS tidak ditemukan.")

    auth_hash = read_hash(AUTH)
    if RECOVERY.exists():
        raise RuntimeError("Recovery Key sudah terdaftar.")

    old = getpass.getpass("Password XENUS sekarang: ")
    if not verify(lib, old, auth_hash):
        raise RuntimeError("Password XENUS salah.")

    key = secrets.token_hex(32)
    recovery_hash = make_hash(lib, key)

    atomic_write(RECOVERY, recovery_hash)

    print("\nRecovery Key berhasil dibuat!")
    print("\nSIMPAN KUNCI INI DI TEMPAT AMAN:")
    print(key)
    print("\nKunci hanya ditampilkan sekarang.")
    print("Jangan simpan di folder proyek atau bagikan kepada orang lain.")


def recover(lib):
    auth_hash = read_hash(AUTH)
    recovery_hash = read_hash(RECOVERY)

    key = getpass.getpass("Recovery Key: ")
    if len(key) != 64 or not verify(lib, key, recovery_hash):
        raise RuntimeError("Recovery Key salah.")

    new = password_input("Password XENUS baru: ")
    confirm = getpass.getpass("Ulangi password baru: ")

    if new != confirm:
        raise RuntimeError("Konfirmasi password tidak cocok.")

    new_hash = make_hash(lib, new)
    atomic_write(AUTH, new_hash)

    print("\nPassword XENUS berhasil direset.")
    print("Recovery Key lama tetap berlaku.")


def main():
    print("=" * 40)
    print("       XENUS RECOVERY TOOL")
    print("=" * 40)
    print("1. Daftarkan Recovery Key")
    print("2. Reset password dengan Recovery Key")

    choice = input("\nPilih [1/2]: ").strip()
    lib = load_lib()

    if not CONFIG.is_dir() or CONFIG.is_symlink():
        raise RuntimeError("Direktori konfigurasi XENUS tidak valid.")

    if choice == "1":
        setup(lib)
    elif choice == "2":
        recover(lib)
    else:
        print("Pilihan tidak valid.")
        return 1

    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, OSError) as exc:
        print(f"\nGAGAL: {exc}")
        sys.exit(1)
