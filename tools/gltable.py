#!/usr/bin/env python3
"""
Writes src/opengl_table.h, the functions Blue's OpenGL libraries export,
from Apple's Mac OS 9 libraries (their exports, and the offset in a context's
dispatch table that each of OpenGLLibrary's gl functions calls through) and
the macOS SDK's OpenGL headers (the C signatures; AGL's and OpenGLMemory's,
which macOS doesn't have, are below).

    tools/gltable.py OpenGLLibrary OpenGLMemory OpenGLUtility > src/opengl_table.h

(the data forks of Mac OS 9.2.2's, as copied from its Extensions folder)
"""
import re
import struct
import subprocess
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from pef import PEF  # noqa: E402

# argument and result types: b GLbyte, B GLubyte and GLboolean, s GLshort, S GLushort, i GLint and GLsizei,
# I GLuint, GLenum and GLbitfield, f GLfloat, d GLdouble, p a pointer, v void (results)
TYPES = {
    'GLenum': 'I', 'GLboolean': 'B', 'GLbitfield': 'I', 'GLbyte': 'b', 'GLshort': 's', 'GLint': 'i',
    'GLsizei': 'i', 'GLubyte': 'B', 'GLushort': 'S', 'GLuint': 'I', 'GLfloat': 'f', 'GLclampf': 'f',
    'GLdouble': 'd', 'GLclampd': 'd', 'void': 'v', 'GLvoid': 'v',
}

AGL = {
    'aglChoosePixelFormat': ('p', 'pip'), 'aglDestroyPixelFormat': ('v', 'p'), 'aglNextPixelFormat': ('p', 'p'),
    'aglDescribePixelFormat': ('B', 'pip'), 'aglDevicesOfPixelFormat': ('p', 'pp'),
    'aglQueryRendererInfo': ('p', 'pi'), 'aglDestroyRendererInfo': ('v', 'p'), 'aglNextRendererInfo': ('p', 'p'),
    'aglDescribeRenderer': ('B', 'pip'), 'aglCreateContext': ('p', 'pp'), 'aglDestroyContext': ('B', 'p'),
    'aglCopyContext': ('B', 'ppI'), 'aglUpdateContext': ('B', 'p'), 'aglSetCurrentContext': ('B', 'p'),
    'aglGetCurrentContext': ('p', ''), 'aglSetDrawable': ('B', 'pp'), 'aglSetOffScreen': ('B', 'piiip'),
    'aglSetFullScreen': ('B', 'piiii'), 'aglGetDrawable': ('p', 'p'), 'aglSetVirtualScreen': ('B', 'pi'),
    'aglGetVirtualScreen': ('i', 'p'), 'aglGetVersion': ('v', 'pp'), 'aglConfigure': ('B', 'II'),
    'aglSwapBuffers': ('v', 'p'), 'aglEnable': ('B', 'pI'), 'aglDisable': ('B', 'pI'), 'aglIsEnabled': ('B', 'pI'),
    'aglSetInteger': ('B', 'pIp'), 'aglGetInteger': ('B', 'pIp'), 'aglUseFont': ('B', 'piBiiii'),
    'aglGetError': ('I', ''), 'aglErrorString': ('p', 'I'), 'aglResetLibrary': ('v', ''),
}

# OpenGLMemory (Mac OS 9 OpenGL SDK's glm.h, as its code uses its arguments)
GLM = {
    'glmMalloc': ('p', 'i'), 'glmCalloc': ('p', 'ii'), 'glmRealloc': ('p', 'pi'), 'glmFree': ('v', 'p'),
    'glmVecAlloc': ('p', 'i'), 'glmVecRealloc': ('p', 'pi'), 'glmVecFree': ('v', 'p'),
    'glmDCBAlloc': ('p', 'i'), 'glmDCBRealloc': ('p', 'pi'), 'glmDCBFree': ('v', 'p'),
    'glmZero': ('v', 'pi'), 'glmCopy': ('v', 'ppi'), 'glmSetUByte': ('v', 'piB'), 'glmSetUShort': ('v', 'piS'),
    'glmSetUInt': ('v', 'piI'), 'glmSetDouble': ('v', 'pid'), 'glmPageFreeAll': ('v', ''),
    'glmGetError': ('I', ''), 'glmSetMode': ('v', 'I'), 'glmSetFunc': ('v', 'Ip'), 'glmSetInteger': ('v', 'Ii'),
    'glmGetInteger': ('i', 'I'), 'glmSetDebugInfo': ('v', 'I'), 'glmReportMemoryStatus': ('v', ''),
    'glmResetMemoryStatus': ('v', ''),
}


def headers():
    sdk = subprocess.run(['xcrun', '--show-sdk-path'], capture_output=True, text=True).stdout.strip()
    h = sdk + '/System/Library/Frameworks/OpenGL.framework/Headers/'
    protos = {}
    for f in ('gl.h', 'glext.h', 'glu.h'):
        src = re.sub(r'\s+', ' ', open(h + f).read())
        for m in re.finditer(r'extern ([\w\s\*]+?)\s*\b(glu?\w+)\s*\(([^)]*(?:\([^)]*\)[^)]*)?)\)', src):
            protos.setdefault(m.group(2), (m.group(1), m.group(3)))
    return protos


def code(t):
    t = t.replace('const ', '').strip()
    if '*' in t or '(' in t:
        return 'p'
    return TYPES[t]


def signature(name, protos):
    ret, args = protos[name]
    args = [a.strip() for a in re.split(r',(?![^()]*\))', args) if a.strip()]
    if args == ['void'] or args == ['GLvoid']:
        args = []
    out = ''
    for a in args:
        if '(' in a:
            out += 'p'
            continue
        a = re.sub(r'\b\w+$', '', a) if not a.endswith('*') and len(a.split()) > 1 else a
        out += code(a)
    return code(ret), out


def main():
    lib, mem, glu = (PEF(open(f, 'rb').read()) for f in sys.argv[1:4])
    protos = headers()
    code_sec, data = lib.sections[0]['data'], lib.sections[1]['data']
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
    gl = []
    for name, cls, value, sec in lib.exports:
        if cls != 2 or not name.startswith('gl'):
            continue
        at = struct.unpack('>I', data[value:value + 4])[0]
        for i in md.disasm(code_sec[at:at + 400], at):
            m = re.match(r'r12, (0x[0-9a-f]+|\d+)\(r\d+\)', i.op_str)
            if i.mnemonic == 'lwz' and m:
                gl.append((int(m.group(1), 0), name))
                break
    gl.sort()
    agl = sorted(e[0] for e in lib.exports if e[0].startswith('agl'))
    print('/*')
    print(' * The functions of Blue\'s OpenGL libraries, as Mac OS 9.2.2\'s export them:')
    print(' * X(name, result, arguments, offset), types as in opengl.c (b GLbyte, B GLubyte')
    print(' * and GLboolean, s GLshort, S GLushort, i GLint and GLsizei, I GLuint, GLenum')
    print(' * and GLbitfield, f GLfloat, d GLdouble, p a pointer, v void), offset where a')
    print(' * context\'s dispatch table has the gl function (0: none). Generated by')
    print(' * tools/gltable.py.')
    print(' */')
    print('#ifndef BLUE_OPENGL_TABLE_H\n#define BLUE_OPENGL_TABLE_H\n')
    print('/* OpenGLLibrary: gl in the order of the dispatch table, then agl */')
    print('#define OPENGL_LIBRARY_FUNCTIONS(X) \\')
    for off, name in gl:
        r, a = signature(name, protos)
        print('\tX(%s, "%s", "%s", 0x%03X) \\' % (name, r, a, off))
    for name in agl:
        r, a = AGL[name]
        print('\tX(%s, "%s", "%s", 0) \\' % (name, r, a))
    print('\n/* OpenGLMemory */')
    print('#define OPENGL_MEMORY_FUNCTIONS(X) \\')
    for name in sorted(e[0] for e in mem.exports):
        r, a = GLM[name]
        print('\tX(%s, "%s", "%s", 0) \\' % (name, r, a))
    print('\n/* OpenGLUtility */')
    print('#define OPENGL_UTILITY_FUNCTIONS(X) \\')
    for name in sorted(e[0] for e in glu.exports):
        r, a = signature(name, protos)
        print('\tX(%s, "%s", "%s", 0) \\' % (name, r, a))
    print('\n#endif')


if __name__ == '__main__':
    main()
