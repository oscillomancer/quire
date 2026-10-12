# Refuses a file in an include root whose name a standard header could
# resolve to.
#
# framework/, product/ and the test directory are on the include path, and
# they are searched before the system headers. macOS and Windows filesystems
# are case-insensitive, so a file there called VERSION is what libc++'s own
# #include <version> opens, and a product/Math.h replaces <math.h> for every
# translation unit in the build -- compile errors deep inside the standard
# library, on those platforms only, never on Linux where the code is
# written. framework/VERSION did exactly this to the first macOS build of
# framework 1.1.0. Checked at configure time so a Linux build catches it too.

set(_supple_std_headers
    # C++ library
    algorithm any array atomic barrier bit bitset charconv chrono codecvt compare complex
    concepts condition_variable coroutine deque exception execution expected filesystem
    flat_map flat_set format forward_list fstream functional future generator
    initializer_list iomanip ios iosfwd iostream istream iterator latch limits list locale
    map mdspan memory memory_resource mutex new numbers numeric optional ostream print queue
    random ranges ratio regex scoped_allocator semaphore set shared_mutex source_location span
    spanstream sstream stack stacktrace stdexcept stdfloat stop_token streambuf string
    string_view strstream syncstream system_error text_encoding thread tuple type_traits
    typeindex typeinfo unordered_map unordered_set utility valarray variant vector version
    # C compatibility
    cassert ccomplex cctype cerrno cfenv cfloat cinttypes ciso646 climits clocale cmath
    csetjmp csignal cstdalign cstdarg cstdbool cstddef cstdint cstdio cstdlib cstring ctgmath
    ctime cuchar cwchar cwctype
    # C
    assert.h complex.h ctype.h errno.h fenv.h float.h inttypes.h iso646.h limits.h locale.h
    math.h setjmp.h signal.h stdalign.h stdarg.h stdatomic.h stdbool.h stddef.h stdint.h
    stdio.h stdlib.h stdnoreturn.h string.h tgmath.h threads.h time.h uchar.h wchar.h wctype.h)

function(supple_check_include_roots)
    foreach(root ${ARGN})
        file(GLOB entries LIST_DIRECTORIES false "${root}/*")
        foreach(entry ${entries})
            get_filename_component(name "${entry}" NAME)
            string(TOLOWER "${name}" lower)
            if (lower IN_LIST _supple_std_headers)
                message(FATAL_ERROR
                    "${entry} shadows the standard header <${lower}> on case-insensitive "
                    "filesystems (macOS, Windows), because ${root} is an include directory. "
                    "Rename it.")
            endif()
        endforeach()
    endforeach()
endfunction()
