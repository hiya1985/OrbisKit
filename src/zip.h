#pragma once
/* Extracts a .zip (stored or deflate entries, no zip64/encryption) into dstdir.
 * Runs inside the job thread: reports progress and errors through g_job. */
int zip_extract(const char *zippath, const char *dstdir);
/* Creates outpath containing the given files/folders (recursively).
 * Entry names are relative to each source's parent folder. */
int zip_create(char **src, int nsrc, const char *outpath);
