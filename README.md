## A debugging journey

An issue was opened https://github.com/r-lib/fs/issues/95 in the [fs]
package describing an unexpected error code (`ENOENT` rather than `EEXIST`)
when copying the same file to the same destination twice. Tracking down the
cause of this turned out to be one of the weirdest bugs I have seen.

The fs package provides filesystem operations for the R language, it is built
on top of filesystem functions in [libuv]. Libuv in
turn relies on the native system calls for each operating system it supports.

The first step in dealing with any bug is reproducing it. The original issue
had a [reprex] (which is great!) and in this case I was able to simplify it to
the following

```r
fs::file_create("a")
fs::file_create("b")

fs::file_copy("a", "b")
#> Error: [ENOENT] Failed to copy 'a' to 'b': no such file or directory
```

The error being the error code `ENOENT`, which corresponds to an `errno` of
`2`. The first interesting bit to this issue is that this incorrect error code
only occurred the _first_ time the copy was attempted in an R session.
Subsequent attempts always returned the proper `EEXIST` error code.

```r
fs::file_create("a")
fs::file_create("b")

try(fs::file_copy("a", "b"))
#> Error : [ENOENT] Failed to copy 'a' to 'b': no such file or directory
try(fs::file_copy("a", "b"))
#> Error : [EEXIST] Failed to copy 'a' to 'b': file already exists
```

Usually if the behavior of code is different the first time it is run it is
caused by failing to initialize an object.
As always the first code to look at when debugging an error is your own, luckily in
this case it had straightforward logic that looked correct. 

[R/copy.R#L43-51](https://github.com/r-lib/fs/blob/0f5b6191935fe4c862d2e5003655e6c1669f4afd/R/copy.R#L43-L51)
```r
file_copy <- function(path, new_path, overwrite = FALSE) {
  # TODO: copy attributes, e.g. cp -p?
  assert_no_missing(path)
  assert_no_missing(new_path)


  copyfile_(path_expand(path), path_expand(new_path), isTRUE(overwrite))


  invisible(path_tidy(new_path))
}
```

[src/file.cc#L280-L304](https://github.com/r-lib/fs/blob/0f5b6191935fe4c862d2e5003655e6c1669f4afd/src/file.cc#L289-L304)
```c++
void copyfile_(CharacterVector path, CharacterVector new_path, bool overwrite) {
  for (R_xlen_t i = 0; i < Rf_xlength(path); ++i) {
    uv_fs_t req;
    const char* p = CHAR(STRING_ELT(path, i));
    const char* n = CHAR(STRING_ELT(new_path, i));
    uv_fs_copyfile(
        uv_default_loop(),
        &req,
        p,
        n,
        !overwrite ? UV_FS_COPYFILE_EXCL : 0,
        NULL);
    stop_for_error2(req, "Failed to copy '%s' to '%s'", p, n);
    uv_fs_req_cleanup(&req);
  }
}
```

Essentially we iterate over the old and new paths in turn and pass them to
`uv_fs_copyfile()`, specifying the `UV_FS_COPYFILE_EXCL` option to indicate we
want to fail the copy if the destination file already exists.

My first thought was there was a race condition when creating the file and
copying, however we observe the same behavior if the files exist prior to
opening the R session, so that hypothesis was quickly ruled out.

The next idea was there was a bug in libuv, so lets go look at the
implementation for `uv_fs_copyfile`. Luckily for us the
implementation it is also very short on macOS.

[libuv/src/unix/fs.c#L787-L797](https://github.com/libuv/libuv/blob/1489c98b7fc17f1702821a269eb0c5e730c5c813/src/unix/fs.c#L787-L797)
```c
static ssize_t uv__fs_copyfile(uv_fs_t* req) {
#if defined(__APPLE__) && !TARGET_OS_IPHONE
  /* On macOS, use the native copyfile(3). */
  copyfile_flags_t flags;


  flags = COPYFILE_ALL;


  if (req->flags & UV_FS_COPYFILE_EXCL)
    flags |= COPYFILE_EXCL;


  return copyfile(req->path, req->new_path, NULL, flags);
```

So this is a very short shim over the system function [copyfile(3)] with
the `COPYFILE_EXCL` flag.

Therefore we can rule out the issue being libuv by calling `copyfile(3)` directly in our package.

```r
void copyfile_(CharacterVector path, CharacterVector new_path, bool overwrite) {
  for (R_xlen_t i = 0; i < Rf_xlength(path); ++i) {
    uv_fs_t req;
    const char* p = CHAR(STRING_ELT(path, i));
    const char* n = CHAR(STRING_ELT(new_path, i));

    int res = copyfile("a", "b", NULL, COPYFILE_ALL | COPYFILE_EXCL);
    Rcpp::Rcout << res << ':' << errno << '\n';
  }
}
```

Unfortunately we see the same behavior from this function the first time it is
called in a new R session, the `errno` is set to `2` (`ENOENT`) instead of `17`
(`EEXIST`) and is `17` as expected the rest of the time. This rules out libuv
as the culprit.

So the next idea, maybe it is a weird interaction with Rcpp or C++? To test
this we can create a new [copyfile] R package, that uses only C and
`copyfile(3)`. Unfortunately after running the resultatnt
`copyfile::copyfile()` in a new session the issue was still present. So this
must not be an issue with C++ or Rcpp.

Next, maybe it is an R issue, lets try reproduce it with a standalone C program.

```c
#include <copyfile.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

int main(int argc, char **argv) {
  errno = 0;
  int res = copyfile(argv[1], argv[2], NULL, COPYFILE_ALL | COPYFILE_EXCL);
  strerror(errno);
  if (res == -1) {
    printf("%i:%i:%s", res, errno, strerror(errno));
  }
  return errno;
}
```

In this case it does _not_ reproduce the error, we always get `17` from `errno`
regardless of which call it is.

So this leaves only a few possibilities. It is a genuine issue in R, an issue in the
implementation of `copyfile(3)`, an issue elsewhere in the OS, or a compiler
bug.

R does not use [`copyfile()`
itself](https://github.com/wch/r-source/search?utf8=%E2%9C%93&q=copyfile&type=)
and does not seem to [change
errno](https://github.com/wch/r-source/search?utf8=%E2%9C%93&q=errno&type=) so
it seemed unlikely to be the culprit.

Apple releases the source code to some of its utilities and system functions,
`copyfile(3)` is one of them, so we can look at the source [copyfile-138].
One thing to note in the source is there is logging being used by the
`copyfile(3)` function. So lets see if the system logs can tell us anything
more.

If we run `log stream --predicate 'processImpagePath ENDSWITH "R"'` we can
capture any system logs from R processes. Then we can run our
`copyfile::copyfile()` function in another window. Doing this gives us this
interesting output from the log.

```
> log stream --predicate 'processImagePath ENDSWITH "R"'
Filtering the log data using "processImagePath ENDSWITH "R""
Timestamp                       Thread     Type        Activity PID
2018-03-30 12:23:45.134161-0400 0x4925ce   Default     0x0      75117  R: (libcopyfile.dylib) open on b: File exists
```

But this output from R.

```r
copyfile::copyfile()
#> -1:2:No such file or directory
```

So the `errno` is set correctly when it is logged (`File exists`), but _changed_
(`No such file or directory`) when `copyfile(3)` returns. The other clue from
the log is the log message "open on b:". If we search for "open on" in the
`copyfile(3)` source code we find two instances.

```c
if ((s->src_fd = open(s->src, O_RDONLY | osrc , 0)) < 0)
{
  copyfile_warn("open on %s", s->src);
  return -1;
}
```

Then a little further down

```c
copyfile_warn("open on %s", s->dst);
return -1;
```

Because `b` is the destination filename that means the latter one
must be where the log occurred. Because the function returns immediately after
the logging and the `errno` is correct when the log is printed _maybe the `errno` is being
changed in the logging function_????

If we look at the `copyfile_warn()` definition we see it is

```c
# define copyfile_warn(str, ...) syslog(LOG_WARNING, str ": %m", ## __VA_ARGS__)
```

Which seems innocuous enough. However exploring the available sources we can see
there are multiple versions of the copyfile source available, if we use a
higher version number [copyfile-146] (aside: does anyone know what macOS versions
correspond to these numbers?) we see the following definition of
`copyfile_warn()`.

```c
// These macros preserve the value of errno.
#ifndef _COPYFILE_TEST
# define copyfile_warn(str, ...) \
	do { \
		errno_t _errsv = errno; \
		syslog(LOG_WARNING, str ": %m", ## __VA_ARGS__); \
		errno = _errsv; \
	} while (0)```
```

So it seems as though the code has been changed at some point to preserve the
`errno`; indicating that previous versions did _not_ preserve this in all cases
exactly the behavior we are seeing!

We can then test this hypothesis by disabling logging for our R process and
seeing if it fixes the bug.

First get the R PID
```r
Sys.getpid()
#> [1] 77310
```

Then disable logging on the command line for that PID
```c
sudo log config --process=77310 --mode 'level:off'
```

Last run our test command

```r
copyfile::copyfile()
#> -1:17:File exists
```

**SUCCESS!!!** With logging turned off we get the expected error number `17` and
proper error message `File exists`.

So after this lengthy exploration this seems like a bug in either the
`copyfile()` or `syslog()` implementations, and may possibly be
fixed in newer versions of macOS than I am using (10.12.6 Sierra). This was
truly one of the weirder bugs I have encountered, which is why I took the time
to document it and I am still not entirely sure the exact cause, but we have
enough evidence to show the likely culprit is the system logging.

[fs]: https://fs.r-lib.org/
[libuv]: http://libuv.org/
[reprex]: http://reprex.tidyverse.org/
[copyfile(3)]: https://developer.apple.com/legacy/library/documentation/Darwin/Reference/ManPages/man3/copyfile.3.html
[copyfile]: https://github.com/copyfile
[copyfile-138]: https://opensource.apple.com/source/copyfile/copyfile-138/copyfile.c.auto.html
[copyfile-146]: https://opensource.apple.com/source/copyfile/copyfile-146/copyfile.c.auto.html
