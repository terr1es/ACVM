# Asserts that the two ways of misusing a string index are refused, each for its own reason.
#
# Both are cases where a program cannot assert anything about itself: one does not compile,
# the other stops the process. So the check runs the compiler and reads what it said.
#
#     cmake -DACVM=<acvm> -DWORK_DIR=<writable dir> -P check_string_index.cmake
#
# What is being pinned:
#
#   * `s[i] = c` must be rejected at compile time. It used to compile and corrupt: the
#     store wrote two bytes through a one-byte element, so `s[0] = 'X'` on "ABCDE" gave
#     "X CDE" -- the high byte of 'X' zeroing the 'B'. Silent, and it looked like it worked.
#
#   * `s[i]` past the end must trap. The bounds check compares against the byte length, and
#     the stride used to be two, so the last two indices of any string read past the
#     allocation and returned whatever was there.
#
#   * a negative index must trap too, and separately: an unsigned comparison in the guard
#     would let it through, reading backwards from the start of the buffer.
#
# The message is checked as well as the failure. A program rejected for the wrong reason
# is a worse outcome than one rejected with no reason, because it sends the reader after a
# bug that is not theirs.

if(NOT DEFINED ACVM OR NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "check_string_index.cmake needs -DACVM=<path> and -DWORK_DIR=<path>")
endif()

# Runs `acvm <mode> <file>` and requires a non-zero exit plus a message fragment in stderr.
function(expect_reject mode case_name source must_contain)
    set(path "${WORK_DIR}/${case_name}.ax")
    file(WRITE "${path}" "${source}")

    execute_process(
        COMMAND "${ACVM}" ${mode} "${path}"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)

    if(rc EQUAL 0)
        message(FATAL_ERROR
            "${case_name}: expected a non-zero exit, but it succeeded.\nstdout:\n${out}")
    endif()
    if(NOT err MATCHES "${must_contain}")
        message(FATAL_ERROR
            "${case_name}: expected stderr to mention '${must_contain}'.\nstderr:\n${err}")
    endif()
endfunction()

# ── compile time: a string element is not assignable ───────────────────────

expect_reject(check string_index_assign [=[
function main():void {
    string s = "ABCDE";
    s[0] = 'X';
    eof: pass;
}
]=] "cannot be assigned to")

# The message has to name the way out, not just the prohibition: a reader who tried to
# overwrite a character wants a string-building path, and str.replace or StrBuilder is it.
expect_reject(check string_index_assign_advice [=[
function main():void {
    string s = "a";
    s[0] = 'b';
    eof: pass;
}
]=] "StrBuilder")

# ── run time: out of range traps ───────────────────────────────────────────

expect_reject(run string_index_past_end [=[
function main():void {
    string s = "abc";
    print(s[3] as int);
    eof: pass;
}
]=] "index out of bounds")

# The last index of a five byte string was one of the two that used to read past the
# allocation, so it is worth naming separately from the generic past-the-end case.
expect_reject(run string_index_last_plus_one [=[
function main():void {
    string s = "1+1+1";
    print(s[5] as int);
    eof: pass;
}
]=] "index out of bounds")

expect_reject(run string_index_negative [=[
function main():void {
    string s = "abc";
    print(s[-1] as int);
    eof: pass;
}
]=] "index out of bounds")

# ── run time: charAt refuses a character that does not fit ─────────────────

# A `char` is one UTF-16 code unit. Truncating U+1F600 would produce a different, valid
# character that nothing downstream could recognise as wrong, so it stops instead and
# says which code point and what to call instead.
expect_reject(run string_charat_outside_bmp [=[
function import (return:(str));
function main():void {
    string grin = "😀";
    char c = charAt(grin, 0);
    print(c);
    eof: pass;
}
]=] "outside the BMP")

expect_reject(run string_charat_advice [=[
function import (return:(str));
function main():void {
    string grin = "😀";
    char c = charAt(grin, 0);
    print(c);
    eof: pass;
}
]=] "codePointAt")

message(STATUS "string index: 7 cases rejected for the stated reason")
