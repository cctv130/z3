############################################
# Copyright (c) 2012 Microsoft Corporation
#
# Scripts for automatically generating
# Window distribution zip files.
#
# Author: Leonardo de Moura (leonardo)
############################################

import os
import subprocess
import zipfile
from mk_exception import *
from mk_project import *
import mk_util

BUILD_DIR = 'build-dist'
BUILD_X64_DIR = os.path.join('build-dist', 'x64')
BUILD_X86_DIR = os.path.join('build-dist', 'x86')
BUILD_ARM64_DIR = os.path.join('build-dist', 'arm64')  # ARM64 build directory
VERBOSE = True
DIST_DIR = 'dist'
FORCE_MK = False
ASSEMBLY_VERSION = None
DOTNET_CORE_ENABLED = True
DOTNET_KEY_FILE = None
JAVA_ENABLED = True
ZIP_BUILD_OUTPUTS = False
GIT_HASH = False
PYTHON_ENABLED = True
X86ONLY = False
X64ONLY = False
ARM64ONLY = False  # ARM64 flag
MAKEJOBS = getenv("MAKEJOBS", "24")


def set_verbose(flag):
    global VERBOSE
    VERBOSE = flag

def is_verbose():
    return VERBOSE

def mk_dir(d):
    if not os.path.exists(d):
        os.makedirs(d)

def set_build_dir(path):
    global BUILD_DIR, BUILD_X86_DIR, BUILD_X64_DIR, BUILD_ARM64_DIR
    BUILD_DIR = mk_util.norm_path(path)
    BUILD_X86_DIR = os.path.join(path, 'x86')
    BUILD_X64_DIR = os.path.join(path, 'x64')
    BUILD_ARM64_DIR = os.path.join(path, 'arm64')  # Set ARM64 build directory
    mk_dir(BUILD_X86_DIR)    
    mk_dir(BUILD_X64_DIR)
    mk_dir(BUILD_ARM64_DIR)

def display_help():
    print("mk_win_dist.py: Z3 Windows distribution generator\n")
    print("This script generates the zip files containing executables, dlls, header files for Windows.")
    print("It must be executed from the Z3 root directory.")
    print("\nOptions:")
    print("  -h, --help                    display this message.")
    print("  -s, --silent                  do not print verbose messages.")
    print("  -b <sudir>, --build=<subdir>  subdirectory where x86 and x64 Z3 versions will be built (default: build-dist).")
    print("  -f, --force                   force script to regenerate Makefiles.")
    print("  --assembly-version            assembly version for dll")
    print("  --nodotnet                    do not include .NET bindings in the binary distribution files.")
    print("  --dotnet-key=<file>           strongname sign the .NET assembly with the private key in <file>.")
    print("  --nojava                      do not include Java bindings in the binary distribution files.")
    print("  --nopython                    do not include Python bindings in the binary distribution files.")
    print("  --zip                         package build outputs in zip file.")
    print("  --githash                     include git hash in the Zip file.")
    print("  --x86-only                    x86 dist only.")
    print("  --x64-only                    x64 dist only.")
    print("  --arm64-only                  arm64 dist only.")
    exit(0)

# Parse configuration option for mk_make script
def parse_options():
    global FORCE_MK, JAVA_ENABLED, ZIP_BUILD_OUTPUTS, GIT_HASH, DOTNET_CORE_ENABLED, DOTNET_KEY_FILE, ASSEMBLY_VERSION, PYTHON_ENABLED, X86ONLY, X64ONLY, ARM64ONLY
    path = BUILD_DIR
    options, remainder = getopt.gnu_getopt(sys.argv[1:], 'b:hsf', ['build=',
                                                                   'help',
                                                                   'silent',
                                                                   'force',
                                                                   'nojava',
                                                                   'nodotnet',
                                                                   'dotnet-key=',
                                                                   'assembly-version=',
                                                                   'zip',
                                                                   'githash',
                                                                   'nopython',
                                                                   'x86-only',
                                                                   'x64-only',
                                                                   'arm64-only'
                                                                   ])
    for opt, arg in options:
        if opt in ('-b', '--build'):
            if arg == 'src':
                raise MKException('The src directory should not be used to host the Makefile')
            path = arg
        elif opt in ('-s', '--silent'):
            set_verbose(False)
        elif opt in ('-h', '--help'):
            display_help()
        elif opt in ('-f', '--force'):
            FORCE_MK = True
        elif opt == '--nodotnet':
            DOTNET_CORE_ENABLED = False
        elif opt == '--assembly-version':
            ASSEMBLY_VERSION = arg
        elif opt == '--nopython':
            PYTHON_ENABLED = False
        elif opt == '--dotnet-key':
            DOTNET_KEY_FILE = arg
        elif opt == '--nojava':
            JAVA_ENABLED = False
        elif opt == '--zip':
            ZIP_BUILD_OUTPUTS = True
        elif opt == '--githash':
            GIT_HASH = True
        elif opt == '--x86-only' and not X64ONLY:
            X86ONLY = True
        elif opt == '--arm64-only' and not X86ONLY and not X64ONLY: 
            ARM64ONLY = True
        elif opt == '--x64-only' and not X86ONLY:
            X64ONLY = True
        else:
            raise MKException("Invalid command line option '%s'" % opt)
    set_build_dir(path)

# Check whether build directory already exists or not
def check_build_dir(path):
    return os.path.exists(path) and os.path.exists(os.path.join(path, 'Makefile'))

# Create a build directory using mk_make.py
def mk_build_dir(path, arch):
    if not check_build_dir(path) or FORCE_MK:
        subprocess.call(["call", "md", path, "2>NUL"], shell=True)

        if arch == "arm64":
            arch = "amd64_arm64"

        opts0 = ["cd", path]

        opts1 = ['"C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise\\VC\\Auxiliary\\Build\\vcvarsall.bat"',  arch]
        
        opts = ["cmake", "-S", "."]
        if DOTNET_CORE_ENABLED:
            opts.append('-DZ3_BUILD_DOTNET_BINDINGS=ON')
            if DOTNET_KEY_FILE is not None:
                opts.append('-DDOTNET_SIGNING_KEY_FILE=' + DOTNET_KEY_FILE)
        if ASSEMBLY_VERSION is not None:
            opts.append('-DZ3_ASSEMBLY_VERSION=' + ASSEMBLY_VERSION)
        if JAVA_ENABLED:
            opts.append('-DZ3_BUILD_JAVA_BINDINGS=ON')
        if GIT_HASH:
            git_hash = mk_util.git_hash()
            opts.append('-DGIT_HASH=' + git_hash)
        if PYTHON_ENABLED:
            opts.append('-DZ3_BUILD_PYTHON_BINDINGS=ON')
        opts.append('-DZ3_USE_LIBGMP=OFF')
        opts.append('-DZ3_BUILD_LIBZ3_SHARED=ON')
        opts.append('-DCMAKE_INSTALL_PREFIX=' + path)
        opts.append('-G "NMake Makefiles"')
        opts.append('../..')
        args = " ".join(opts0) + "& " + " ".join(opts1) + "& " + " ".join(opts)
        print(args)
        if subprocess.call(args, shell=True) != 0:
            raise MKException("Failed to generate build directory at '%s'" % path)


# Create build directories
def mk_build_dirs():
    mk_build_dir(BUILD_X86_DIR, 'x86')
    mk_build_dir(BUILD_X64_DIR, 'x64')
    mk_build_dir(BUILD_ARM64_DIR, 'arm64')  # ARM64 build directory creation

# Check if on Visual Studio command prompt
def check_vc_cmd_prompt():
    try:
        DEVNULL = open(os.devnull, 'wb')
        subprocess.call(['cl'], stdout=DEVNULL, stderr=DEVNULL)
    except:
        raise MKException("You must execute the mk_win_dist.py script on a Visual Studio Command Prompt")

def exec_cmds(cmds):
    cmd_file = 'z3_tmp.cmd'
    f = open(cmd_file, 'w')
    for cmd in cmds:
        f.write(cmd)
        f.write('\n')
    f.close()
    res = 0
    try:
        res = subprocess.call(cmd_file, shell=True)
    except:
        res = 1
    try:
        os.erase(cmd_file)
    except:
        pass
    return res

def get_build_dir(arch):
    if arch == 'x64':
        return BUILD_X64_DIR
    if arch == 'x86':
        return BUILD_X86_DIR
    return BUILD_ARM64_DIR

def mk_z3(arch):
    build_dir = get_build_dir(arch)
    if arch == "arm64":
        arch = "x64_arm64"
    cmds = []
    cmds.append('call "%VCINSTALLDIR%Auxiliary\\build\\vcvarsall.bat" ' + arch + ' ')
    cmds.append('cd %s' % build_dir)
    cmds.append('nmake')
    if exec_cmds(cmds) != 0:
        raise MKException("Failed to make z3, x64: %s" % x64)

def mk_z3s():
    mk_z3('x86')
    mk_z3('x64')
    mk_z3('arm64')

def get_z3_name(arch):
    major, minor, build, revision = get_version()
    print("Assembly version:", major, minor, build, revision)
    platform = arch
    if GIT_HASH:
        return 'z3-%s.%s.%s.%s-%s-win' % (major, minor, build, mk_util.git_hash(), platform)
    else:
        return 'z3-%s.%s.%s-%s-win' % (major, minor, build, platform)

def mk_dist_dir(arch):
    build_path = get_build_dir(arch)
    dist_path = os.path.join(DIST_DIR, get_z3_name(arch))
    mk_dir(dist_path)
    mk_win_dist(build_path, dist_path)
    if is_verbose():
        print(f"Generated {platform} distribution folder at '{dist_path}'")
        
def mk_dist_dirs():
    mk_dist_dir("x86")
    mk_dist_dir("x64")
    mk_dist_dir("arm64")

def get_dist_path(arch):
    return get_z3_name(arch)

def mk_zip(arch):
    dist_path = get_dist_path(arch)
    old = os.getcwd()
    try:
        os.chdir(DIST_DIR)
        zfname = '%s.zip' % dist_path
        zipout = zipfile.ZipFile(zfname, 'w', zipfile.ZIP_DEFLATED)
        for root, dirs, files in os.walk(dist_path):
            for f in files:
                zipout.write(os.path.join(root, f))
        if is_verbose():
            print("Generated '%s'" % zfname)
    except:
        pass
    os.chdir(old)

# Create a zip file for each platform
def mk_zips():
    mk_zip(False)
    mk_zip(True)


VS_RUNTIME_PATS = [re.compile(r'vcomp.*\.dll'),
                   re.compile(r'msvcp.*\.dll'),
                   re.compile(r'msvcr.*\.dll'),
                   re.compile(r'vcrun.*\.dll')]

# Copy Visual Studio Runtime libraries
def cp_vs_runtime(arch):
    platform = arch
    vcdir = os.environ['VCINSTALLDIR']
    path  = '%sredist' % vcdir
    vs_runtime_files = []
    print("Walking %s" % path)
    # Everything changes with every release of VS
    # Prior versions of VS had DLLs under "redist\x64"
    # There are now several variants of redistributables
    # The naming convention defies my understanding so 
    # we use a "check_root" filter to find some hopefully suitable
    # redistributable.
    def check_root(root):
        return platform in root and ("CRT" in root or "MP" in root) and "onecore" not in root and "debug" not in root
    for root, dirs, files in os.walk(path):
        for filename in files:
            if fnmatch(filename, '*.dll') and check_root(root):
                print("Checking %s %s" % (root, filename))
                for pat in VS_RUNTIME_PATS:
                    if pat.match(filename):
                        fname = os.path.join(root, filename)
                        if not os.path.isdir(fname):
                            vs_runtime_files.append(fname)
    if not vs_runtime_files:
        raise MKException("Did not find any runtime files to include")       
    bin_dist_path = os.path.join(DIST_DIR, get_dist_path(arch), 'bin')
    for f in vs_runtime_files:
        shutil.copy(f, bin_dist_path)
        if is_verbose():
            print("Copied '%s' to '%s'" % (f, bin_dist_path))

def cp_vs_runtimes():
    cp_vs_runtime("x86")
    cp_vs_runtime("x64")
    cp_vs_runtime("arm64")

def cp_license(arch):
    shutil.copy("LICENSE.txt", os.path.join(DIST_DIR, get_dist_path(arch)))

def cp_licenses():
    cp_license("x86")
    cp_license("x64")
    cp_license("arm64")

def init_flags():
    global DOTNET_KEY_FILE, JAVA_ENABLED, PYTHON_ENABLED, ASSEMBLY_VERSION
    mk_util.DOTNET_CORE_ENABLED = True
    mk_util.DOTNET_KEY_FILE = DOTNET_KEY_FILE
    mk_util.ASSEMBLY_VERSION = ASSEMBLY_VERSION
    mk_util.JAVA_ENABLED = JAVA_ENABLED
    mk_util.PYTHON_ENABLED = PYTHON_ENABLED
    mk_util.ALWAYS_DYNAMIC_BASE = True


# Entry point
def main():
    if os.name != 'nt':
        raise MKException("This script is for Windows only")

    parse_options()
    check_vc_cmd_prompt()
    init_flags()

    if X86ONLY:
        mk_build_dir(BUILD_X86_DIR, 'x86')
        mk_z3('x86')
        init_project_def()
        mk_dist_dir('x86')
        cp_license('x86')
        cp_vs_runtime('x86')
        if ZIP_BUILD_OUTPUTS:
            mk_zip('x86')
    elif X64ONLY:
        mk_build_dir(BUILD_X64_DIR, 'x64')
        mk_z3('x64')
        init_project_def()
        mk_dist_dir('x64')
        cp_license('x64')
        cp_vs_runtime('x64')
        if ZIP_BUILD_OUTPUTS:
            mk_zip('x64')
    elif ARM64ONLY:  # ARM64 build process
        mk_build_dir(BUILD_ARM64_DIR, 'arm64')
        mk_z3('arm64')
        init_project_def()
        mk_dist_dir('arm64')
        cp_license('arm64')
        cp_vs_runtime('arm64')
        if ZIP_BUILD_OUTPUTS:
            mk_zip('arm64')

    else:
        mk_build_dirs()
        mk_z3s()
        init_project_def()
        mk_dist_dirs()
        cp_licenses()
        cp_vs_runtimes()
        if ZIP_BUILD_OUTPUTS:
            mk_zips()

main()

