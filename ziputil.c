#ifdef __APPLE__
	#define HANDLE_XATTRS /* compress/decompress file extended attributes
				 and resource forks */
#endif

#include <sys/types.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <utime.h>
#include <time.h>
#include <limits.h>
#include <stdlib.h>
#include "zip.h"
#include "unzip.h"
#include "ziputil.h"
#ifdef HANDLE_XATTRS
	#include <copyfile.h>
#endif

#define BUFFER_SIZE (1024*16)

/*
 *  Create directories for relative path with its parents starting
 *  from base directory.
 *
 *  relative should end with filename or slash (for directory)
 *  base can be NULL
 */
int make_dirs(const char *base, const char *relative)
{
	char *path, *p;
	struct stat st;
	size_t len;
	
	if (!base) { /* base can be NULL, in this case start with / */
		base = "/";
		st.st_mode = 0;
	} else {
		stat(base, &st); /* it's ok to fail */		
	}
	/* Skip leading slash in relativePath */
	p = (char *)relative;
	if (p[0] == '/')
		p++;
	/* The following will alloc path + 2 spare bytes for \0 and slash
	 * (if we need it) 
	 */
	path = malloc(strlen(base) + strlen(p) + 2);
	if (!path)
		return 0;
	strcpy(path, base);
	/* If there's no ending slash, add it */
	len = strlen(path);
	if (path[len-1] != '/') {
		path[len] = '/';
		path[++len] = '\0';
	}
	strcat(path, p); /* now contains full path to the directory we need to create */
	p = path + len;
	while ((p = strchr(p, '/'))) {
		*p = '\0'; /* this will replace / in path as well */
		if (mkdir(path, st.st_mode) == -1) {
			if (errno == EEXIST) {
				/* if directory exists, get its mode */
				stat(path, &st);	
			} else {
				/* error! */
				free(path);
				return 0;
			}
		}
		*p++ = '/';	/* restore slash and move p to next char */
	}
	free(path);
	return 1;
}


/*
 *  Combine paths to one string, adding slashes as needed.
 *  List of paths should end with NULL.
 *
 *  It doesn't add trailing slash to the last path.
 *
 *  Examples:
 *
 *  combine_paths(path, "one", "two", "three", NULL)
 *      path ---> /one/two/three
 *
 *  combine_paths(path, "one", "two", "three", "", NULL)
 *      path ---> /one/two/three/
 *
 *  combine_paths(path, "one/", "two/", "three", NULL)
 *      path ---> /one/two/three
 */
int combine_paths(char *path, ...)
{
	va_list ap;
	char *s;
	int len;
	
	va_start(ap, path);
	strcpy(path, "/");
	while((s = va_arg(ap, char *)) != NULL) {
		len = strlen(path);
		if (len > 0) {
			if (len > PATH_MAX-2) {
				strcpy(path, "\0");
				va_end(ap);
				fprintf(stderr, "combine_paths: path too big\n");
				return 0;
			}
			/* Check for trailing slash */
			if (path[len-1] != '/')
				strcat(path, "/");
		}
		if (s[0] == '/')
			strcat(path, s+1); /* copy without slash */
		else
			strcat(path, s);
	}          
	va_end(ap);
	return 1;
}


static int compress_file(const zipFile zf, const char *relative_path, 
			 const char *full_path, int level, void *read_buf);

/*
 *  Checks if file extension is one of the defined for no compression.
 *  Because we better don't compress already compressed files.
 */
const char *ZIP_RAW_EXTENTIONS = ".png|.zip|.gz|.mpg|.mov|.rar|";
inline static int no_compression_file_ext(const char *filename)
{
	char *ext = strrchr(filename, '.');
	if (ext != NULL) {
		char *ep = strstr(ZIP_RAW_EXTENTIONS, ext);
		if (ep != NULL && *(ep+strlen(ext)) == '|')
			return 1;
	}
	return 0;
}

inline static void filetime(const char *filePath, zip_fileinfo *zi)
{
	struct stat s;
	struct tm* filedate;
	time_t tm_t=0;
	
	if (lstat(filePath,&s)==0)
		tm_t = s.st_mtime;
	filedate = localtime(&tm_t);
	
	zi->tmz_date.tm_sec  = filedate->tm_sec;
	zi->tmz_date.tm_min  = filedate->tm_min;
	zi->tmz_date.tm_hour = filedate->tm_hour;
	zi->tmz_date.tm_mday = filedate->tm_mday;
	zi->tmz_date.tm_mon  = filedate->tm_mon ;
	zi->tmz_date.tm_year = filedate->tm_year + 1900; 
			       /* Mac OS X's stat() returns dates from 1900 */
	zi->external_fa = (uLong)s.st_mode << 16;
}

#ifdef HANDLE_XATTRS

static char *pack_appledouble_file(const char *path)
{
	char *tmp_file = strdup("/tmp/crzip.temp.XXXXXX");
	
	if (!mktemp(tmp_file) || 
	    (copyfile(path, tmp_file, NULL, COPYFILE_NOFOLLOW_SRC | COPYFILE_PACK | 
		     COPYFILE_ACL | COPYFILE_XATTR) < 0)) {
		free(tmp_file);
		return NULL;
	}
	return tmp_file;
}

static int unpack_appledouble_file(const char *appledouble_path, 
				   const char *target_path)
{
	return (copyfile(appledouble_path, target_path, NULL, 
			COPYFILE_NOFOLLOW_SRC | COPYFILE_UNPACK | 
			COPYFILE_ACL | COPYFILE_XATTR) == 0);
}


static int compress_xattrs(const zipFile zf, const char *relative_path,
			   const char *full_path, int level, void *read_buf)
{
	char appledouble_path[PATH_MAX], dirname[PATH_MAX], filename[PATH_MAX];
	char *tmp_file, *p;
	int success = 1;

	tmp_file = pack_appledouble_file(full_path);
	if (!tmp_file)
		return 1; /* ok, no extended attributes */
	
	strcpy(appledouble_path, "__MACOSX/");
	strcpy(filename, "._");
	/* add ._ to the last path component */
	strcpy(dirname, relative_path);
	p = strrchr(dirname, '/');
	if (p) {
		strcat(filename, ++p);
		*p = '\0';
		strcat(appledouble_path, dirname);
		strcat(appledouble_path, filename);
	} else {
		strcat(filename, relative_path);
		strcat(appledouble_path, filename);
	}
	success = compress_file(zf, appledouble_path, tmp_file, level, read_buf);
	unlink(tmp_file);
	free(tmp_file);
	return success;
}

#endif /* HANDLE_XATTRS */

static int compress_file(const zipFile zf, const char *relative_path, 
			 const char *full_path, int level, void *read_buf)
{
	zip_fileinfo zi;
	FILE *fin;
	size_t size_read;
	int success = 1;
	
	zi.tmz_date.tm_sec = zi.tmz_date.tm_min = zi.tmz_date.tm_hour =
	zi.tmz_date.tm_mday = zi.tmz_date.tm_mon = zi.tmz_date.tm_year = 0;
	zi.dosDate = 0;
	zi.internal_fa = 0;
	zi.external_fa = 0;
	
	filetime(full_path, &zi);
	
	/* Zip symbolic links */
	if (S_ISLNK(zi.external_fa >> 16)) {
		success = 1;
		size_read = readlink(full_path, read_buf, BUFFER_SIZE);
		if (size_read <= 0) {
			success = 0;	
		} else {
			if (zipOpenNewFileInZip2(zf, relative_path, &zi, NULL, 0, NULL, 0, NULL, 0, 0, 0) != ZIP_OK)
				return 0;
			if (zipWriteInFileInZip(zf, read_buf, size_read) != ZIP_OK)
				success = 0;
		}	
		zipCloseFileInZip(zf);
		return success;		
	}	
	
	/* Put file as raw if it has extension for compressed file */
	if (no_compression_file_ext(relative_path))
		level = 0;
	
	if (zipOpenNewFileInZip2(zf, relative_path, &zi, NULL, 0, NULL, 0, 
				 NULL, Z_DEFLATED, level, 0) != ZIP_OK)
		return 0;
	
	/* Zip actual files */
	fin = fopen(full_path, "r");
	if (!fin) {
		zipCloseFileInZip(zf);
		return 0;
	}
	//fcntl(/*open...*/, F_NOCACHE, 1); /* turn caching off for file */
	do {
		size_read = fread(read_buf, 1, BUFFER_SIZE, fin);
		if (size_read < BUFFER_SIZE && feof(fin) == 0) {
			success = 0;
			break;
		}			
		if (size_read > 0) {
			if (zipWriteInFileInZip(zf, read_buf, size_read) != ZIP_OK) {
				success = 0;
				break;
			}
		}
	} while (size_read > 0);		
	fclose(fin);
	zipCloseFileInZip(zf);
	return success;
}

static int compress_directory(zipFile zf, const char *relative_path, 
			      const char *full_path, int level, void *read_buf)
{
	DIR *dir;
	struct dirent *ent;
	char new_relative_path[PATH_MAX], new_full_path[PATH_MAX];
	int success = 0;

	if (!relative_path)
		relative_path = "";
	dir = opendir(full_path);
	if (!dir)
		return 0;
	while ((ent = readdir(dir)) != NULL) {
		if ((ent->d_name[0] == '.') && (ent->d_name[1] == 0 ||
		    ((ent->d_name[1] == '.') && (ent->d_name[2] == 0))))
			continue;
		if (!combine_paths(new_relative_path, relative_path, 
				   ent->d_name, NULL))
			goto cleanup;
		if (!combine_paths(new_full_path, full_path, ent->d_name, NULL))
			goto cleanup;
		if (ent->d_type == DT_DIR) {
			strcat(new_relative_path, "/");
			strcat(new_full_path, "/");
			if (!compress_directory(zf, new_relative_path, new_full_path,
						level, read_buf))
				goto cleanup;				
		} else if (ent->d_type == DT_REG || ent->d_type == DT_LNK) {
			if (!compress_file(zf, new_relative_path, new_full_path, 
					   level, read_buf))
				goto cleanup;
#ifdef HANDLE_XATTRS
			if (!compress_xattrs(zf, new_relative_path, new_full_path, 
					     level, read_buf))
				goto cleanup;
#endif /* HANDLE_XATTRS */
			
		}
	}
	success = 1;
cleanup:
	closedir(dir);
	return success;
}

static void get_last_path_component(const char *path, char *component)
{
	char *tmp, *p;
	size_t len;
	
	if (!path || !component)
		return;
	tmp = strdup(path);
	if (!tmp)
		return;
	len = strlen(tmp);
	if (tmp[len-1] == '/')
		tmp[len-1] = 0;
	p = strrchr(tmp, '/');
	if (p)
		strcpy(component, ++p);	
	free(tmp);
}

int compress_path(const char *source, const char *zipfile, int level)
{
	struct stat st;
	void *read_buf;
	int success = 0;;
	char relative_path[PATH_MAX];
	zipFile zf;
	
	if (lstat(source, &st) == -1)
		return 0;

	zf = zipOpen(zipfile, 0);
	if (!zf)
		return 0;
	read_buf = malloc(BUFFER_SIZE);
	if (!read_buf)
		goto cleanup;
	get_last_path_component(source, relative_path);

	if (S_ISDIR(st.st_mode)) {
		strcat(relative_path, "/");
		success = compress_directory(zf, relative_path, source, level, read_buf);
		/* Note: if you don't want to zip first level folder name, pass NULL 
		 * instead of relative_path */
	}
	else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
		success = compress_file(zf, relative_path, source, level, read_buf);
#ifdef HANDLE_XATTRS
		if (success)
			success = compress_xattrs(zf, relative_path, source, 
						  level, read_buf);
#endif /* HANDLE_XATTRS */
	}
cleanup:
	zipClose(zf, NULL);
	free(read_buf);
	return success;
}

int compress_path_preserving_time(const char *source, const char *zipfile, int level)
{
	struct stat st;
	struct utimbuf buf;
	
	lstat(source, &st);
	if (!compress_path(source, zipfile, level))
		return 0;
	buf.actime = st.st_atime;
	buf.modtime = st.st_mtime;
	if (utime(zipfile, &buf) == -1) {
		perror("compress_path_preservingtime: cannot set time");
	}
	return 1;
}

static int unzip_current_file(unzFile unzf, const char *destination_dir, void *read_buf)
{
	int success = 0;
	unz_file_info uzi;
	char filename[PATH_MAX];
	char destfile[PATH_MAX];
	FILE *fout;
	size_t size_read;
	
	if (unzGetCurrentFileInfo(unzf, &uzi, filename, sizeof(filename), 
				  NULL, 0, NULL, 0) != UNZ_OK)
		return 0;
	
	/* extract data from zipped file */
	strcpy(destfile, destination_dir);
	strcat(destfile, filename);
	
	/* Skip directories */
	uzi.external_fa = uzi.external_fa >> 16;
	if (S_ISDIR(uzi.external_fa))
		return 1;
	
	if (unzOpenCurrentFile(unzf) != UNZ_OK)
		return 0;
	
	/* Handle symlinks */
	
	if (S_ISLNK(uzi.external_fa)) {
		success = 1;
		size_read = unzReadCurrentFile(unzf, read_buf, BUFFER_SIZE-1); 
		/* -1 to get space for \0 */
		if (size_read > 0) {
			((char *)read_buf)[size_read] = '\0';
			symlink(read_buf, destfile);
		} else {
			success = 0;
		}
		unzCloseCurrentFile(unzf);
		return success;	
	}
	
	/* Handle files */
#ifdef HANDLE_XATTRS
	char tmp[PATH_MAX], real_destfile[PATH_MAX];
	char *p;
	int unzipping_appledouble = 0;

	if ((filename[0] == '_') && (strstr(filename, "__MACOSX/") == filename)) {
		unzipping_appledouble = 1;
		strcpy(tmp, filename + 9  /* __MACOSX/ */ );
		p = strrchr(tmp, '/'); // hello/._test, p=> /._test
		if (p) {
			*p = 0; /* tmp now has directory name */
			p++;
		} else {
			p = tmp;
		}
		if (strlen(p) > 2 && p[0] == '.' && p[1] == '_') {
			strcpy(real_destfile, destination_dir);
			if (p != tmp) {
				strcat(real_destfile, tmp);
				strcat(real_destfile, "/");
			}
			p += 2; /* p now has file name */
			strcat(real_destfile, p);
			
			/* Create empty destfile if it doesn't exist */
			fout = fopen(real_destfile, "r");
			if (!fout)
				fopen(real_destfile, "w");
			fclose(fout);
			 
			char *tmp_file = strdup("/tmp/crunzip.temp.XXXXXX");
			
			if (!mktemp(tmp_file)) {
				unzCloseCurrentFile(unzf);
				return 0;
			}
			/* set destfile (target for unzipping) to temp name */
			strcpy(destfile, tmp_file);
			free(tmp_file);
			goto skip_makedirs;
		}
	}
	
#endif /* HANDLE_XATTRS */
	
	/* Create intermediate directories for file */
	if (!make_dirs(destination_dir, filename))
		return 0;
	
#ifdef HANDLE_XATTRS
skip_makedirs:	
#endif	/* HANDLE_XATTRS */
	
	fout = fopen(destfile, "w"); 
	if (!fout) {
		unzCloseCurrentFile(unzf);
		return 0;
	}
	
	do {
		size_read = unzReadCurrentFile(unzf, read_buf, BUFFER_SIZE);
		if (size_read > 0) {
			if (fwrite(read_buf, 1, size_read, fout) != size_read)
				goto cleanup; /* error writing */
		}
	} while (size_read > 0);
	success = 1;
	if (uzi.external_fa > 0)
		chmod(destfile, uzi.external_fa);
cleanup:
	fclose(fout);
	unzCloseCurrentFile(unzf);
	
#ifdef HANDLE_XATTRS
	if (unzipping_appledouble) {
		success = unpack_appledouble_file(destfile, real_destfile);
		unlink(destfile); /* remove temp appledouble file */
	}
#endif /* HANDLE_XATTRS */
	
	return success;
}

int decompress_path(const char *zipfile, const char *destination)
{
	int success = 0;
	unzFile unzf; /* it's a pointer already. stupid unzip.h! */
	int next_result;
	void *read_buf;
	
	/* NOTE: destination must end with slash */
	if (!make_dirs(NULL, destination))
		return 0;	
	unzf = unzOpen(zipfile);
	if (!unzf)
		return 0;
	read_buf = malloc(BUFFER_SIZE);
	if (!read_buf) {
		unzClose(unzf);
		return 0;
	}
	if (unzGoToFirstFile(unzf) != UNZ_OK)
		goto cleanup;
	do {
		if (!unzip_current_file(unzf, destination, read_buf))
			goto cleanup;
	} while ((next_result = unzGoToNextFile(unzf)) == UNZ_OK);
	
	success = (next_result == UNZ_END_OF_LIST_OF_FILE);
cleanup:
	free(read_buf);
	unzClose(unzf);
	return success;
}
