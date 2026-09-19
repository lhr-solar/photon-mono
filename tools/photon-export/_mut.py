import pathlib, subprocess, sys, importlib.util

spec = importlib.util.spec_from_file_location("rc", "revert_check.py")
rc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rc)

label_sub = sys.argv[1]
build = pathlib.Path("../../.artifacts").resolve()
exe = build / "tools" / "photon-export" / "photon-export-tests.exe"
for label, f, needle, repl, exp in rc.MUTATIONS:
    if label_sub not in label:
        continue
    p = pathlib.Path(f)
    orig = p.read_text(encoding="utf-8")
    p.write_text(orig.replace(needle, repl), encoding="utf-8")
    try:
        b = subprocess.run(["cmake", "--build", str(build), "--target", "photon-export-tests"],
                           capture_output=True, text=True)
        if b.returncode != 0:
            print("BUILD FAILED\n" + b.stdout[-3000:] + b.stderr[-3000:])
        else:
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            print("---- stdout ----\n" + r.stdout)
            print("---- stderr ----\n" + r.stderr[-6000:])
    finally:
        p.write_text(orig, encoding="utf-8")
subprocess.run(["cmake", "--build", str(build), "--target", "photon-export-tests"],
               capture_output=True, text=True)
