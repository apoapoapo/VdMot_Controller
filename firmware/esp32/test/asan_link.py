# Link the sanitizer runtimes into the native test binary.
Import("env")  # noqa: F821
env.Append(LINKFLAGS=["-fsanitize=address,undefined"])  # noqa: F821
