#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <GL/gl.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define WM_REPORT_PIXEL_FORMAT (WM_APP + 1)
#define WM_BEGIN_TRANSITIONS (WM_APP + 2)
#define TRANSITION_TIMER 1
#define MINIMIZED_SWAP_COUNT 8
#define RESTORE_DELAY_MS 2500
#define TIMING_FRAMES 600

static int reported_pixel_format;
static HANDLE transition_event;
static HANDLE restore_event;

typedef BOOL (WINAPI *wgl_swap_interval_proc)( int interval );

static int compare_u64( const void *left, const void *right )
{
    const uint64_t a = *(const uint64_t *)left;
    const uint64_t b = *(const uint64_t *)right;

    return a > b ? 1 : a < b ? -1 : 0;
}

static uint64_t percentile_us( uint64_t *samples, unsigned int count,
                               unsigned int numerator, uint64_t frequency )
{
    unsigned int index;

    qsort( samples, count, sizeof(*samples), compare_u64 );
    index = (count * numerator + 99) / 100;
    if (!index) index = 1;
    return (uint64_t)(((__uint128_t)samples[index - 1] * 1000000u + frequency / 2) /
                      frequency);
}

static LRESULT CALLBACK window_proc( HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam )
{
    if (message == WM_REPORT_PIXEL_FORMAT)
    {
        reported_pixel_format = wparam;
        return 0;
    }
    if (message == WM_BEGIN_TRANSITIONS)
    {
        RECT rect;

        GetWindowRect( hwnd, &rect );
        SetWindowPos( hwnd, NULL, rect.left + 40, rect.top + 30,
                      480, 320, SWP_NOACTIVATE | SWP_NOZORDER );
        ShowWindow( hwnd, SW_MINIMIZE );
        SetEvent( transition_event );
        SetTimer( hwnd, TRANSITION_TIMER, RESTORE_DELAY_MS, NULL );
        return 0;
    }
    if (message == WM_TIMER && wparam == TRANSITION_TIMER)
    {
        KillTimer( hwnd, TRANSITION_TIMER );
        ShowWindow( hwnd, SW_RESTORE );
        SetEvent( restore_event );
        return 0;
    }
    return DefWindowProcW( hwnd, message, wparam, lparam );
}

static int child_main( HWND hwnd, HANDLE start_event, HANDLE restored_event )
{
    PIXELFORMATDESCRIPTOR pfd = {0};
    HGLRC context = NULL;
    HDC dc = NULL;
    wgl_swap_interval_proc swap_interval;
    PROC swap_interval_address;
    LARGE_INTEGER frequency, counter;
    uint64_t samples[TIMING_FRAMES], previous = 0;
    int format, i, minimized_failures = 0;
    int ret = 1;

    dc = GetDC( hwnd );
    if (!dc)
    {
        fprintf( stderr, "GetDC failed: %lu\n", GetLastError() );
        goto done;
    }

    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;

    if (!(format = ChoosePixelFormat( dc, &pfd )))
    {
        fprintf( stderr, "ChoosePixelFormat failed: %lu\n", GetLastError() );
        goto done;
    }
    if (!SetPixelFormat( dc, format, &pfd ))
    {
        fprintf( stderr, "SetPixelFormat failed: %lu\n", GetLastError() );
        goto done;
    }
    if (GetPixelFormat( dc ) != format)
    {
        fprintf( stderr, "foreign GetPixelFormat did not preserve format %d\n", format );
        goto done;
    }

    if (!(context = wglCreateContext( dc )))
    {
        fprintf( stderr, "wglCreateContext failed: %lu\n", GetLastError() );
        goto done;
    }
    if (!wglMakeCurrent( dc, context ))
    {
        fprintf( stderr, "wglMakeCurrent failed: %lu\n", GetLastError() );
        goto done;
    }
    glViewport( 0, 0, 128, 128 );

    swap_interval_address = wglGetProcAddress( "wglSwapIntervalEXT" );
    _Static_assert( sizeof(swap_interval) == sizeof(swap_interval_address),
                    "OpenGL procedure pointer size mismatch" );
    memcpy( &swap_interval, &swap_interval_address, sizeof(swap_interval) );
    if (!swap_interval || !swap_interval( 1 ))
    {
        fprintf( stderr, "wglSwapIntervalEXT(1) is unavailable: %lu\n", GetLastError() );
        goto done;
    }
    if (!QueryPerformanceFrequency( &frequency ) || frequency.QuadPart <= 0)
    {
        fprintf( stderr, "QueryPerformanceFrequency failed\n" );
        goto done;
    }

    for (i = 0; i < 3; ++i)
    {
        glClearColor( i == 0, i == 1, i == 2, 1.0f );
        glClear( GL_COLOR_BUFFER_BIT );
        if (!SwapBuffers( dc ))
        {
            fprintf( stderr, "SwapBuffers failed: %lu\n", GetLastError() );
            goto done;
        }
        if (GetPixelFormat( dc ) != format)
        {
            fprintf( stderr, "foreign pixel format was lost after present %d\n", i );
            goto done;
        }
        if (!SetPixelFormat( dc, format, &pfd ))
        {
            fprintf( stderr, "setting the preserved pixel format failed: %lu\n", GetLastError() );
            goto done;
        }
    }

    PostMessageW( hwnd, WM_BEGIN_TRANSITIONS, 0, 0 );
    if (WaitForSingleObject( start_event, 5000 ) != WAIT_OBJECT_0)
    {
        fprintf( stderr, "owner did not start window-state transitions\n" );
        goto done;
    }
    /* The candidate's hidden-window policy intentionally spaces these calls
     * at 250 ms.  Keep the owner minimized long enough that every iteration,
     * rather than only the first few, exercises the non-presentable path. */
    for (i = 0; i < MINIMIZED_SWAP_COUNT; ++i)
    {
        glClear( GL_COLOR_BUFFER_BIT );
        if (!SwapBuffers( dc )) ++minimized_failures;
    }
    if (WaitForSingleObject( restored_event, 5000 ) != WAIT_OBJECT_0)
    {
        fprintf( stderr, "owner did not finish window-state transitions\n" );
        goto done;
    }
    for (i = 0; i < TIMING_FRAMES; ++i)
    {
        glClearColor( (i & 1) != 0, (i & 2) != 0, (i & 4) != 0, 1.0f );
        glClear( GL_COLOR_BUFFER_BIT );
        if (!SwapBuffers( dc ))
        {
            fprintf( stderr, "SwapBuffers failed during transition stress: %lu\n",
                     GetLastError() );
            goto done;
        }
        QueryPerformanceCounter( &counter );
        samples[i] = previous ? counter.QuadPart - previous : 0;
        previous = counter.QuadPart;
    }
    memmove( samples, samples + 1, (TIMING_FRAMES - 1) * sizeof(*samples) );
    printf( "foreign_wgl_scheduler_metrics frames=%u interval_p50_us=%" PRIu64
            " interval_p95_us=%" PRIu64 " interval_p99_us=%" PRIu64
            " minimized_swap_failures=%d\n",
            TIMING_FRAMES - 1,
            percentile_us( samples, TIMING_FRAMES - 1, 50, frequency.QuadPart ),
            percentile_us( samples, TIMING_FRAMES - 1, 95, frequency.QuadPart ),
            percentile_us( samples, TIMING_FRAMES - 1, 99, frequency.QuadPart ),
            minimized_failures );

    if (!wglMakeCurrent( NULL, NULL ))
    {
        fprintf( stderr, "clearing the WGL context failed: %lu\n", GetLastError() );
        goto done;
    }
    if (!ReleaseDC( hwnd, dc ))
    {
        fprintf( stderr, "ReleaseDC failed: %lu\n", GetLastError() );
        dc = NULL;
        goto done;
    }
    dc = GetDC( hwnd );
    if (!dc || GetPixelFormat( dc ) != format)
    {
        fprintf( stderr, "foreign pixel format was lost across DC acquisition\n" );
        goto done;
    }
    if (minimized_failures)
    {
        fprintf( stderr, "%d SwapBuffers calls failed while minimized\n",
                 minimized_failures );
        goto done;
    }

    PostMessageW( hwnd, WM_REPORT_PIXEL_FORMAT, format, 0 );
    printf( "Foreign WGL pixel format %d remained stable across presents.\n", format );
    ret = 0;

done:
    wglMakeCurrent( NULL, NULL );
    if (context) wglDeleteContext( context );
    if (dc) ReleaseDC( hwnd, dc );
    return ret;
}

static int parent_main( const WCHAR *executable )
{
    WNDCLASSW class = {0};
    PROCESS_INFORMATION process = {0};
    STARTUPINFOW startup = {0};
    WCHAR command[2048];
    DWORD exit_code = 1;
    HWND hwnd = NULL;
    HDC dc = NULL;
    MSG message;
    int ret = 1;

    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), NULL, TRUE};

    class.lpfnWndProc = window_proc;
    class.hInstance = GetModuleHandleW( NULL );
    class.lpszClassName = L"SwitchyardForeignWglTest";
    if (!RegisterClassW( &class ))
    {
        fprintf( stderr, "RegisterClass failed: %lu\n", GetLastError() );
        goto done;
    }
    hwnd = CreateWindowW( class.lpszClassName, L"Switchyard foreign WGL test",
                          WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                          320, 240, NULL, NULL, class.hInstance, NULL );
    if (!hwnd)
    {
        fprintf( stderr, "CreateWindow failed: %lu\n", GetLastError() );
        goto done;
    }

    if (!(transition_event = CreateEventW( &attributes, TRUE, FALSE, NULL )))
    {
        fprintf( stderr, "CreateEvent failed: %lu\n", GetLastError() );
        goto done;
    }
    if (!(restore_event = CreateEventW( &attributes, TRUE, FALSE, NULL )))
    {
        fprintf( stderr, "CreateEvent failed: %lu\n", GetLastError() );
        goto done;
    }

    startup.cb = sizeof(startup);
    swprintf( command, sizeof(command) / sizeof(command[0]),
              L"\"%ls\" --child 0x%llx 0x%llx 0x%llx", executable,
              (unsigned long long)(ULONG_PTR)hwnd,
              (unsigned long long)(ULONG_PTR)transition_event,
              (unsigned long long)(ULONG_PTR)restore_event );
    if (!CreateProcessW( NULL, command, NULL, NULL, TRUE, 0, NULL, NULL,
                         &startup, &process ))
    {
        fprintf( stderr, "CreateProcess failed: %lu\n", GetLastError() );
        goto done;
    }

    for (;;)
    {
        DWORD wait = MsgWaitForMultipleObjects( 1, &process.hProcess, FALSE,
                                                30000, QS_ALLINPUT );

        while (PeekMessageW( &message, NULL, 0, 0, PM_REMOVE ))
        {
            TranslateMessage( &message );
            DispatchMessageW( &message );
        }
        if (wait == WAIT_OBJECT_0) break;
        if (wait != WAIT_OBJECT_0 + 1)
        {
            fprintf( stderr, "child process wait failed or timed out: %lu\n", wait );
            TerminateProcess( process.hProcess, 2 );
            goto done;
        }
    }
    while (PeekMessageW( &message, NULL, 0, 0, PM_REMOVE ))
    {
        TranslateMessage( &message );
        DispatchMessageW( &message );
    }

    if (!GetExitCodeProcess( process.hProcess, &exit_code ) || exit_code)
    {
        fprintf( stderr, "foreign WGL child failed with status %lu\n", exit_code );
        goto done;
    }
    if (!reported_pixel_format)
    {
        fprintf( stderr, "foreign WGL child did not report a pixel format\n" );
        goto done;
    }
    dc = GetDC( hwnd );
    if (!dc || GetPixelFormat( dc ) != reported_pixel_format)
    {
        fprintf( stderr, "owner did not receive foreign pixel format %d\n",
                 reported_pixel_format );
        goto done;
    }
    printf( "Foreign WGL owner received pixel format %d.\n", reported_pixel_format );
    ret = 0;

done:
    if (hwnd)
    {
        KillTimer( hwnd, TRANSITION_TIMER );
        ShowWindow( hwnd, SW_RESTORE );
    }
    if (dc) ReleaseDC( hwnd, dc );
    if (process.hThread) CloseHandle( process.hThread );
    if (process.hProcess) CloseHandle( process.hProcess );
    if (transition_event) CloseHandle( transition_event );
    if (restore_event) CloseHandle( restore_event );
    if (hwnd) DestroyWindow( hwnd );
    if (class.lpszClassName) UnregisterClassW( class.lpszClassName, class.hInstance );
    return ret;
}

int wmain( int argc, WCHAR **argv )
{
    if (argc == 5 && !wcscmp( argv[1], L"--child" ))
        return child_main( (HWND)(ULONG_PTR)_wcstoui64( argv[2], NULL, 0 ),
                           (HANDLE)(ULONG_PTR)_wcstoui64( argv[3], NULL, 0 ),
                           (HANDLE)(ULONG_PTR)_wcstoui64( argv[4], NULL, 0 ) );
    return parent_main( argv[0] );
}
