#' @useDynLib copyfile, .registration = TRUE
NULL

copyfile <- function() {
  invisible(.Call(copyfile_))
}
