/*
 *  ziputil.h
 *  OSXZip
 *
 *  DEPENDENCIES: Link against zlib (-lz).
 *
 *  Created by Dmitry Chestnykh on 12.07.08.
 *  Copyright (C) 2008 Coding Robots
 *
 *  Email  : dmitry@codingrobots.com 
 *  Website: http://www.codingrobots.com
 *
 *  Distributed under zlib license:
 *
 *  This software is provided 'as-is', without any express or implied
 *  warranty.  In no event will the authors be held liable for any damages
 *  arising from the use of this software.
 * 
 *  Permission is granted to anyone to use this software for any purpose,
 *  including commercial applications, and to alter it and redistribute it
 *  freely, subject to the following restrictions:
 * 
 *  1. The origin of this software must not be misrepresented; you must not
 *  claim that you wrote the original software. If you use this software
 *  in a product, an acknowledgment in the product documentation would be
 *  appreciated but is not required.
 *  2. Altered source versions must be plainly marked as such, and must not be
 *  misrepresented as being the original software.
 *  3. This notice may not be removed or altered from any source distribution.
 *
 */

#ifndef ZIP_ZIPUTIL_H
#define ZIP_ZIPUTIL_H

int compress_path(const char *source, const char *zipfile, int level);
int compress_path_preserving_time(const char *source, const char *zipfile, int level);
int decompress_path(const char *zipfile, const char *destination);

#endif /* ZIP_ZIPUTIL_H */