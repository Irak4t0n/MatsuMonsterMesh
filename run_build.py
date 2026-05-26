"""Wrapper to run the IDF build from any shell by spawning cmd.exe properly."""
import subprocess, sys, os

# Strip MSys/MinGW environment variables that cause idf.py to abort
env = dict(os.environ)
for key in list(env.keys()):
    if key.upper() in ('MSYSTEM', 'MSYSTEM_CARCH', 'MSYSTEM_CHOST',
                        'MSYSTEM_PREFIX', 'MINGW_CHOST', 'MINGW_PREFIX',
                        'MINGW_PACKAGE_PREFIX', 'SHELL', 'SHLVL',
                        'TERM', 'TERM_PROGRAM', 'TERM_PROGRAM_VERSION'):
        del env[key]

bat = r"C:\Users\Howar\MatsuMonsterMesh\reconfigure.bat"
result = subprocess.run(
    ["cmd.exe", "/c", bat],
    stdout=sys.stdout,
    stderr=sys.stderr,
    cwd=r"C:\Users\Howar\MatsuMonsterMesh",
    env=env,
)
sys.exit(result.returncode)
