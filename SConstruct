import sys, os.path

class ConfigurationError(Exception):
    def __init__(self, reason):
        Exception.__init__(self, "Configuration failed: %s" % reason)

def _PackageOption(pkgName, default="yes"):
    return BoolOption("use%s" % pkgName[0].upper() + pkgName[1:], "use %s if available" % (pkgName), default)

def _BoolOption(opt, explanation, default="yes"):
    return BoolOption("enable%s" % opt[0].upper() + opt[1:], explanation, default)

def _DirectoryOption(path, help, default):
    return PathOption(path, help, default, PathOption.PathIsDir)

def getPlatform():
    global __platform
    try: __platform
    except NameError:
        env = Environment()
        if env["PLATFORM"] == "posix":
            if sys.platform[:5] == "linux":
                return "linux"
            if sys.platform[:6] == "darwin":
                return "darwin"
            else:
                # What does this identifier look like for SGI?
                raise ConfigurationError("Unknown platform %s" % sys.platform)
        if env["PLATFORM"] == "win32":
            return "win32"
        if env["PLATFORM"] == "cygwin":
            return "cygwin"
        else:
            raise ConfigurationError("Unknown platform %s" % env["PLATFORM"])

Posix = ("linux", "darwin")
env = Environment(CPPPATH="pa_common")

Platform = getPlatform()

opts = Options("options.cache", args=ARGUMENTS)
if Platform in Posix:
    opts.AddOptions(
            _DirectoryOption("prefix", "installation prefix", "/usr/local"),
            _PackageOption("ALSA"),
            _PackageOption("OSS"),
            _PackageOption("JACK"),
            )

opts.AddOptions(
        _BoolOption("shared", "create shared library"),
        _BoolOption("static", "create static library"),
        _BoolOption("debug", "compile with debug symbols"),
        _BoolOption("optimize", "compile with optimization", default="no"),
        _BoolOption("debugOutput", "enable debug output", default="no"),
        ("customCFlags", "customize compilation of C code", ""),
        )

opts.Update(env)
opts.Save("options.cache", env)

Help(opts.GenerateHelpText(env))

env.SConscriptChdir(False)
SConscript("SConscript", build_dir=".build_scons", exports=["env", "Platform", "Posix", "ConfigurationError"], duplicate=False)
