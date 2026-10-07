/*
 * ARM64EC compiler-generated memcpy contract
 *
 * Copyright 2026 Switchyard Wine project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * In addition to the permissions in the GNU Lesser General Public
 * License, the authors give you unlimited permission to link the
 * compiled version of this file with other programs, and to distribute
 * those programs without any restriction coming from the use of this
 * file.  (The GNU Lesser General Public License restrictions do apply
 * in other respects; for example, they cover modification of the file,
 * and distribution when not linked into another program.)
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifdef __arm64ec__

typedef __SIZE_TYPE__ size_t;

__asm__(".weak __imp_memcpy");

__declspec(dllimport) void *__cdecl memcpy( void *, const void *, size_t );

/* LLVM's ARM64EC call-lowering pass runs before late memory-intrinsic libcall
 * expansion.  Keep the IAT reference a weak alias so it cannot pull an unused
 * import; a real import overrides it and receives this compiler-authored exit
 * thunk. */
static void * __attribute__((used, noinline, nodebug, no_builtin("memcpy")))
arm64ec_memcpy_contract( void *dst, const void *src, size_t size )
{
    return memcpy( dst, src, size );
}

#endif /* __arm64ec__ */
