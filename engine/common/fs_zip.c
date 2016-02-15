#include "quakedef.h"
#include "fs.h"

#ifdef AVAIL_ZLIB
#define ZIPCRYPT

#ifndef ZEXPORT
	#define ZEXPORT VARGS
#endif
#include <zlib.h>

typedef struct {
	unsigned char ident1;
	unsigned char ident2;
	unsigned char cm;
	unsigned char flags;
	unsigned int mtime;
	unsigned char xflags;
	unsigned char os;
	//unsigned short xlen;
	//unsigned char xdata[xlen];
	//unsigned char fname[];
	//unsigned char fcomment[];
	//unsigned short fhcrc;
	//unsigned char compresseddata[];
	//unsigned int crc32;
	//unsigned int isize;
} gzheader_t;
#define sizeofgzheader_t 10

#define	GZ_FTEXT	1
#define	GZ_FHCRC	2
#define GZ_FEXTRA	4
#define GZ_FNAME	8
#define GZ_FCOMMENT	16
#define GZ_RESERVED (32|64|128)

#ifdef DYNAMIC_ZLIB
#define ZLIB_LOADED() (zlib_handle!=NULL)
void *zlib_handle;
#define ZSTATIC(n)
#else
#define ZLIB_LOADED() 1
#define ZSTATIC(n) = &n

#ifdef _MSC_VER
# ifdef _WIN64
# pragma comment(lib, MSVCLIBSPATH "zlib64.lib")
# else
# pragma comment(lib, MSVCLIBSPATH "zlib.lib")
# endif
#endif
#endif

#ifndef Z_U4
#define z_crc_t uLongf
#endif

//#pragma comment(lib, MSVCLIBSPATH "zlib.lib")

static int (ZEXPORT *qinflateEnd) (z_streamp strm) ZSTATIC(inflateEnd);
static int (ZEXPORT *qinflate) (z_streamp strm, int flush) ZSTATIC(inflate);
static int (ZEXPORT *qinflateInit2_) (z_streamp strm, int  windowBits,
                                      const char *version, int stream_size) ZSTATIC(inflateInit2_);
//static z_crc_t (ZEXPORT *qcrc32)   (uLong crc, const Bytef *buf, uInt len) ZSTATIC(crc32);
#ifdef ZIPCRYPT
static const z_crc_t *(ZEXPORT *qget_crc_table)   (void) ZSTATIC(get_crc_table);
#endif

#define qinflateInit2(strm, windowBits) \
        qinflateInit2_((strm), (windowBits), ZLIB_VERSION, sizeof(z_stream))

qboolean LibZ_Init(void)
{
	#ifdef DYNAMIC_ZLIB
	static dllfunction_t funcs[] =
	{
		{(void*)&qinflateEnd,		"inflateEnd"},
		{(void*)&qinflate,			"inflate"},
		{(void*)&qinflateInit2_,	"inflateInit2_"},
//		{(void*)&qcrc32,			"crc32"},
#ifdef ZIPCRYPT
		{(void*)&qget_crc_table,	"get_crc_table"},
#endif
		{NULL, NULL}
	};
	if (!ZLIB_LOADED())
		zlib_handle = Sys_LoadLibrary("zlib1", funcs);
	#endif
	return ZLIB_LOADED();
}

//outfile may be null
vfsfile_t *FS_DecompressGZip(vfsfile_t *infile, vfsfile_t *outfile)
{
	char inchar;
	unsigned short inshort;
	vfsfile_t *temp;
	gzheader_t header;

	if (VFS_READ(infile, &header, sizeofgzheader_t) == sizeofgzheader_t)
	{
		if (header.ident1 != 0x1f || header.ident2 != 0x8b || header.cm != 8 || header.flags & GZ_RESERVED)
		{
			VFS_SEEK(infile, 0);
			return infile;
		}
	}
	else
	{
		VFS_SEEK(infile, 0);
		return infile;
	}
	if (header.flags & GZ_FEXTRA)
	{
		VFS_READ(infile, &inshort, sizeof(inshort));
		inshort = LittleShort(inshort);
		VFS_SEEK(infile, VFS_TELL(infile) + inshort);
	}

	if (header.flags & GZ_FNAME)
	{
		Con_Printf("gzipped file name: ");
		do {
			if (VFS_READ(infile, &inchar, sizeof(inchar)) != 1)
				break;
			Con_Printf("%c", inchar);
		} while(inchar);
		Con_Printf("\n");
	}

	if (header.flags & GZ_FCOMMENT)
	{
		Con_Printf("gzipped file comment: ");
		do {
			if (VFS_READ(infile, &inchar, sizeof(inchar)) != 1)
				break;
			Con_Printf("%c", inchar);
		} while(inchar);
		Con_Printf("\n");
	}

	if (header.flags & GZ_FHCRC)
	{
		VFS_READ(infile, &inshort, sizeof(inshort));
	}



	if (outfile)
		temp = outfile;
	else
	{
		temp = FS_OpenTemp();
		if (!temp)
		{
			VFS_SEEK(infile, 0);	//doh
			return infile;
		}
	}


	{
		unsigned char inbuffer[16384];
		unsigned char outbuffer[16384];
		int ret;

		z_stream strm = {
			inbuffer,
			0,
			0,

			outbuffer,
			sizeof(outbuffer),
			0,

			NULL,
			NULL,

			NULL,
			NULL,
			NULL,

			Z_UNKNOWN,
			0,
			0
		};

		strm.avail_in = VFS_READ(infile, inbuffer, sizeof(inbuffer));
		strm.next_in = inbuffer;

		qinflateInit2(&strm, -MAX_WBITS);

		while ((ret=qinflate(&strm, Z_SYNC_FLUSH)) != Z_STREAM_END)
		{
			if (strm.avail_in == 0 || strm.avail_out == 0)
			{
				if (strm.avail_in == 0)
				{
					strm.avail_in = VFS_READ(infile, inbuffer, sizeof(inbuffer));
					strm.next_in = inbuffer;
					if (!strm.avail_in)
						break;
				}

				if (strm.avail_out == 0)
				{
					strm.next_out = outbuffer;
					VFS_WRITE(temp, outbuffer, strm.total_out);
					strm.total_out = 0;
					strm.avail_out = sizeof(outbuffer);
				}
				continue;
			}

			//doh, it terminated for no reason
			if (ret != Z_STREAM_END)
			{
				qinflateEnd(&strm);
				Con_Printf("Couldn't decompress gz file\n");
				VFS_CLOSE(temp);
				VFS_CLOSE(infile);
				return NULL;
			}
		}
		//we got to the end
		VFS_WRITE(temp, outbuffer, strm.total_out);

		qinflateEnd(&strm);

		if (temp->Seek)
			VFS_SEEK(temp, 0);
	}
	VFS_CLOSE(infile);

	return temp;
}










//returns an unseekable write-only file that will decompress any data that is written to it.
//decompressed data will be written to the output
typedef struct
{
	vfsfile_t vf;
	vfsfile_t *outfile;
	qboolean autoclosefile;

	//gzip header handling
	qboolean headerparsed;
	qbyte in[65536];
	int inlen;

	qbyte out[65536];
	z_stream strm;
} vf_gz_dec_t;

static qboolean QDECL FS_GZ_Dec_Close(vfsfile_t *f)
{
	vf_gz_dec_t *n = (vf_gz_dec_t*)f;
	if (n->autoclosefile)
		VFS_CLOSE(n->outfile);
	qinflateEnd(&n->strm);
	Z_Free(n);
	return true;
}
static int QDECL FS_GZ_Dec_Write(vfsfile_t *f, const void *buffer, int len)
{
	int ret;
	vf_gz_dec_t *n = (vf_gz_dec_t*)f;

	if (n->headerparsed != 1)
	{
		qofs_t ofs = 0;
		gzheader_t *header;
		int chunk;
		if (len > sizeof(n->in) - n->inlen)
			chunk = sizeof(n->in) - n->inlen;
		else
			chunk = len;
		memmove(n->in, buffer, chunk);
		n->inlen += chunk;

		if (n->headerparsed == 2)
		{
			if (n->inlen >= 8)
			{
				//unsigned int crc = (n->in[0]<<0) | (n->in[1]<<8) | (n->in[2]<<16) | (n->in[3]<<24);
				unsigned int isize = (n->in[4]<<0) | (n->in[5]<<8) | (n->in[6]<<16) | (n->in[7]<<24);
				if (n->strm.total_out != isize)
					return -1;	//the file we just received decoded to a different length (yay, concat...).
				//FIXME: validate the decoded crc
				n->headerparsed = false;
				n->strm.total_in = 0;
				n->strm.total_out = 0;
				len = n->inlen - 8;
				n->inlen = 0;
				if (FS_GZ_Dec_Write(f, n->in + 8, len) != len)
					return -1;	//FIXME: make non-fatal somehow
				return chunk;
			}
			return chunk;
		}

		header = (gzheader_t*)n->in;
		ofs += sizeofgzheader_t;  if (ofs > n->inlen) goto noheader;

		if (header->ident1 != 0x1f || header->ident2 != 0x8b || header->cm != 8 || header->flags & GZ_RESERVED)
			return -1;	//ERROR

		if (header->flags & GZ_FEXTRA)
		{
			unsigned short ex;
			if (ofs+2 > n->inlen) goto noheader;
			ex = (n->in[ofs]<<0) | (n->in[ofs+1]<<8);	//read little endian
			ofs += 2+ex;  if (ofs > n->inlen) goto noheader;
		}

		if (header->flags & GZ_FNAME)
		{
			char ch;
//			Con_Printf("gzipped file name: ");
			do {
				if (ofs+1 > n->inlen) goto noheader;
				ch = n->in[ofs++];
//				Con_Printf("%c", ch);
			} while(ch);
//			Con_Printf("\n");
		}

		if (header->flags & GZ_FCOMMENT)
		{
			char ch;
//			Con_Printf("gzipped file comment: ");
			do {
				if (ofs+1 > n->inlen) goto noheader;
				ch = n->in[ofs++];
//				Con_Printf("%c", ch);
			} while(ch);
//			Con_Printf("\n");
		}

		if (header->flags & GZ_FHCRC)
		{
			ofs += 2;  if (ofs > n->inlen) goto noheader;
		}

		//if we got here then the header is now complete.
		//tail-recurse it.
		n->headerparsed = true;
		chunk = n->inlen - ofs;
		if (FS_GZ_Dec_Write(f, n->in + ofs, chunk) != chunk)
			return -1;	//FIXME: make non-fatal somehow
		return len;

noheader:
		if (n->inlen == sizeof(n->in))
			return -1;	//we filled our buffer. if we didn't get past the header yet then someone fucked up.
		return len;
	}


	n->strm.next_in = (char*)buffer;
	n->strm.avail_in = len;

	while(n->strm.avail_in)
	{
		ret = qinflate(&n->strm, Z_SYNC_FLUSH);

		if (!n->strm.avail_out)
		{
			if (VFS_WRITE(n->outfile, n->out, n->strm.next_out-n->out) != n->strm.next_out-n->out)
				return -1;

			n->strm.next_out = n->out;
			n->strm.avail_out = sizeof(n->out);
		}

		if (ret == Z_OK)
			continue;

		//flush it
		if (VFS_WRITE(n->outfile, n->out, n->strm.next_out-n->out) != n->strm.next_out-n->out)
			return -1;
		n->strm.next_out = n->out;
		n->strm.avail_out = sizeof(n->out);

		if (ret == Z_STREAM_END)	//allow concat
		{
			int l = n->strm.avail_in;
			n->headerparsed = 2;
			n->inlen = 0;
			if (l != FS_GZ_Dec_Write(f, n->strm.next_in, n->strm.avail_in))
				return -1;
			return len;
		}

		switch (ret)
		{
		case Z_NEED_DICT:
			Con_Printf("Z_NEED_DICT\n");
			break;

		case Z_ERRNO:
			Con_Printf("Z_ERRNO\n");
			break;

		case Z_STREAM_ERROR:
		case Z_DATA_ERROR:
		case Z_MEM_ERROR:
		case Z_BUF_ERROR:
		case Z_VERSION_ERROR:
			Con_Printf("File is corrupt\n");
			break;
		default:
			Con_Printf("Bug!\n");
			break;
		}
		return -1;
	}

	return len;
}

vfsfile_t *FS_GZ_DecompressWriteFilter(vfsfile_t *outfile, qboolean autoclosefile)
{
	vf_gz_dec_t *n = Z_Malloc(sizeof(*n));

	n->outfile = outfile;
	n->autoclosefile = autoclosefile;

	n->strm.next_in		= NULL;
	n->strm.avail_in 	= 0;
	n->strm.total_in	= 0;
	n->strm.next_out	= n->out;
	n->strm.avail_out 	= sizeof(n->out);
	n->strm.total_out	= 0;

	n->vf.Flush				= NULL;
	n->vf.GetLen			= NULL;
	n->vf.ReadBytes			= NULL;
	n->vf.Seek				= NULL;
	n->vf.Tell				= NULL;
	n->vf.Close				= FS_GZ_Dec_Close;
	n->vf.WriteBytes		= FS_GZ_Dec_Write;
	n->vf.seekingisabadplan	= true;

	qinflateInit2(&n->strm, -MAX_WBITS);

	return &n->vf;
}


#endif















typedef struct
{
	fsbucket_t bucket;
	char	name[MAX_QPATH];
	qofs_t	localpos;	//location of local header
	qofs_t	filelen;	//uncompressed size
	time_t	mtime;
	unsigned int		crc;
	unsigned int		flags;
} zpackfile_t;
#define ZFL_DEFLATED	1	//need to use zlib
#define ZFL_STORED		2	//direct access is okay
#define ZFL_SYMLINK		4	//file is a symlink
#define ZFL_CORRUPT		8	//file is corrupt or otherwise unreadable.
#define ZFL_WEAKENCRYPT	16	//traditional zip encryption


typedef struct zipfile_s
{
	searchpathfuncs_t pub;

	char			filename[MAX_OSPATH];
	unsigned int	numfiles;
	zpackfile_t		*files;

	//info about the underlying file
	void			*mutex;
	qofs_t			curpos;	//cache position to avoid excess seeks
	qofs_t			rawsize;
	vfsfile_t		*raw;

	int				references;	//number of files open inside, so things don't crash if is closed in the wrong order.
} zipfile_t;


static void QDECL FSZIP_GetPathDetails(searchpathfuncs_t *handle, char *out, size_t outlen)
{
	zipfile_t *zip = (void*)handle;

	if (zip->references != 1)
		Q_snprintfz(out, outlen, "(%i)", zip->references-1);
	else
		*out = '\0';
}
static void QDECL FSZIP_ClosePath(searchpathfuncs_t *handle)
{
	qboolean stillopen;
	zipfile_t *zip = (void*)handle;

	if (!Sys_LockMutex(zip->mutex))
		return;	//ohnoes
	stillopen = --zip->references > 0;
	Sys_UnlockMutex(zip->mutex);
	if (stillopen)
		return;	//not free yet

	VFS_CLOSE(zip->raw);
	Sys_DestroyMutex(zip->mutex);
	if (zip->files)
		Z_Free(zip->files);
	Z_Free(zip);
}
static void QDECL FSZIP_BuildHash(searchpathfuncs_t *handle, int depth, void (QDECL *AddFileHash)(int depth, const char *fname, fsbucket_t *filehandle, void *pathhandle))
{
	zipfile_t *zip = (void*)handle;
	int i;

	for (i = 0; i < zip->numfiles; i++)
	{
		if (zip->files[i].flags & ZFL_CORRUPT)
			continue;
		AddFileHash(depth, zip->files[i].name, &zip->files[i].bucket, &zip->files[i]);
	}
}
static unsigned int QDECL FSZIP_FLocate(searchpathfuncs_t *handle, flocation_t *loc, const char *filename, void *hashedresult)
{
	zpackfile_t *pf = hashedresult;
	int i;
	zipfile_t	*zip = (void*)handle;
	int ret = FF_NOTFOUND;

// look through all the pak file elements

	if (pf)
	{	//is this a pointer to a file in this pak?
		if (pf < zip->files || pf >= zip->files + zip->numfiles)
			return FF_NOTFOUND;	//was found in a different path
	}
	else
	{
		for (i=0 ; i<zip->numfiles ; i++)	//look for the file
		{
			if (!Q_strcasecmp (zip->files[i].name, filename))
			{
				if (zip->files[i].flags & ZFL_CORRUPT)
					continue;
				pf = &zip->files[i];
				break;
			}
		}
	}

	if (pf)
	{
		ret = FF_FOUND;
		if (loc)
		{
			loc->index = pf - zip->files;
			*loc->rawname = 0;
			loc->offset = (qofs_t)-1;
			loc->len = pf->filelen;

			if (pf->flags & ZFL_SYMLINK)
				ret = FF_SYMLINK;

	//		if (unzLocateFileMy (zip->handle, loc->index, zip->files[loc->index].filepos) == 2)
	//			ret = FF_SYMLINK;
	//		loc->offset = unzGetCurrentFileUncompressedPos(zip->handle);
//			if (loc->offset<0)
//			{	//file not found, or is compressed.
//				*loc->rawname = '\0';
//				loc->offset=0;
//			}
		}
		else
			ret = FF_FOUND;
		return ret;
	}
	return FF_NOTFOUND;
}

static vfsfile_t *QDECL FSZIP_OpenVFS(searchpathfuncs_t *handle, flocation_t *loc, const char *mode);
static void QDECL FSZIP_ReadFile(searchpathfuncs_t *handle, flocation_t *loc, char *buffer)
{
	vfsfile_t *f;
	f = FSZIP_OpenVFS(handle, loc, "rb");
	if (!f)	//err...
		return;
	VFS_READ(f, buffer, loc->len);
	VFS_CLOSE(f);
}

static int QDECL FSZIP_EnumerateFiles (searchpathfuncs_t *handle, const char *match, int (QDECL *func)(const char *, qofs_t, time_t mtime, void *, searchpathfuncs_t *spath), void *parm)
{
	zipfile_t *zip = (void*)handle;
	int		num;

	for (num = 0; num<(int)zip->numfiles; num++)
	{
		if (wildcmp(match, zip->files[num].name))
		{
			if (!func(zip->files[num].name, zip->files[num].filelen, zip->files[num].mtime, parm, &zip->pub))
				return false;
		}
	}

	return true;
}

static int QDECL FSZIP_GeneratePureCRC(searchpathfuncs_t *handle, int seed, int crctype)
{
	zipfile_t *zip = (void*)handle;

	int result;
	int *filecrcs;
	int numcrcs=0;
	int i;

	filecrcs = BZ_Malloc((zip->numfiles+1)*sizeof(int));
	filecrcs[numcrcs++] = seed;

	for (i = 0; i < zip->numfiles; i++)
	{
		if (zip->files[i].filelen>0)
			filecrcs[numcrcs++] = zip->files[i].crc;
	}

	if (crctype || numcrcs < 1)
		result = Com_BlockChecksum(filecrcs, numcrcs*sizeof(int));
	else
		result = Com_BlockChecksum(filecrcs+1, (numcrcs-1)*sizeof(int));

	BZ_Free(filecrcs);
	return result;
}

#ifdef AVAIL_ZLIB
struct decompressstate
{
	zipfile_t *source;
	qofs_t cstart;	//position of start of data
	qofs_t cofs;	//current read offset
	qofs_t cend;	//compressed data ends at this point.
	qofs_t usize;	//data remaining. refuse to read more bytes than this.
	unsigned char inbuffer[16384];
	unsigned char outbuffer[16384];
	unsigned int readoffset;

#ifdef ZIPCRYPT
	qboolean encrypted;
	unsigned int cryptkey[3];
	unsigned int initialkey[3];
	const int * crctable;
#endif

	z_stream strm;
};

#ifdef ZIPCRYPT
#define CRC32(c, b) ((*(st->crctable+(((int)(c) ^ (b)) & 0xff))) ^ ((c) >> 8))

static void FSZIP_UpdateKeys(struct decompressstate *st, unsigned char ch)
{
	st->cryptkey[0] = CRC32(st->cryptkey[0], ch);
	st->cryptkey[1] += (st->cryptkey[0] & 0xffu);
	st->cryptkey[1] = st->cryptkey[1] * 0x8088405u + 1;
	ch = st->cryptkey[1] >> 24;
	st->cryptkey[2] = CRC32(st->cryptkey[2], ch);
}
static unsigned char FSZIP_DecryptByte(struct decompressstate *st)
{
	unsigned int temp;
	temp = (st->cryptkey[2]&0xffff) | 2;
	return ((temp * (temp ^ 1)) >> 8) & 0xff;
}

static qboolean FSZIP_SetupCrytoKeys(struct decompressstate *st, const char *password, char *cryptheader, unsigned int crc)
{
	unsigned int u;
	st->crctable = qget_crc_table();
	st->encrypted = true;
	st->cryptkey[0] = 0x12345678;
	st->cryptkey[1] = 0x23456789;
	st->cryptkey[2] = 0x34567890;
	while (*password)
		FSZIP_UpdateKeys(st, *password++);

	for (u = 0; u < 12; u++)
	{
		unsigned char ch = cryptheader[u] ^ FSZIP_DecryptByte(st);
		FSZIP_UpdateKeys(st, ch);
		cryptheader[u] = ch;
	}

	memcpy(st->initialkey, st->cryptkey, sizeof(st->initialkey));

	//cryptheader[11] should be the high byte of the file's crc
	//[10] might be the second byte, but also might not be.
	if (cryptheader[11] != (unsigned char)(crc>>24))
		return false;
//	if (cryptheader[10] != (unsigned char)(crc>>16))
//		return false;
	return true;
}
static void FSZIP_DecryptBlock(struct decompressstate *st, char *block, size_t blocksize)
{
    while (blocksize--)
	{
        unsigned char temp = *block ^ FSZIP_DecryptByte(st);
        FSZIP_UpdateKeys(st, temp);
        *block++ = temp;
	}
}
#endif

static struct decompressstate *FSZIP_Decompress_Init(zipfile_t *source, qofs_t start, qofs_t csize, qofs_t usize, char *filename, char *password, unsigned int crc)
{
	struct decompressstate *st;
	if (!ZLIB_LOADED())
	{
		Con_Printf("zlib not available\n");
		return NULL;
	}
	st = Z_Malloc(sizeof(*st));
	st->source = source;

#ifdef ZIPCRYPT
	if (password && csize >= 12)
	{
		char entropy[12];
		if (Sys_LockMutex(source->mutex))
		{
			VFS_SEEK(source->raw, start);
			VFS_READ(source->raw, entropy, sizeof(entropy));
			Sys_UnlockMutex(source->mutex);
		}
		if (!FSZIP_SetupCrytoKeys(st, password, entropy, crc))
		{
			Con_Printf("Invalid password, cannot decrypt %s\n", filename);
			Z_Free(st);
			return NULL;
		}
		start += sizeof(entropy);
		csize -= sizeof(entropy);
	}
#endif

	st->cstart = st->cofs = start;
	st->cend = start + csize;
	st->usize = usize;
	st->strm.data_type = Z_UNKNOWN;
	qinflateInit2(&st->strm, -MAX_WBITS);
	return st;
}

static qofs_t FSZIP_Decompress_Read(struct decompressstate *st, qbyte *buffer, qofs_t bytes)
{
	qboolean eof = false;
	int err;
	qofs_t read = 0;
	while(bytes)
	{
		if (st->readoffset < st->strm.total_out)
		{
			unsigned int consume = st->strm.total_out-st->readoffset;
			if (consume > bytes)
				consume = bytes;
			memcpy(buffer, st->outbuffer+st->readoffset, consume);
			buffer += consume;
			bytes -= consume;
			read += consume;
			st->readoffset += consume;
			continue;
		}
		else if (eof)
			break;	//no input available, and nothing in the buffers.

		st->strm.total_out = 0;
		st->strm.avail_out = sizeof(st->outbuffer);
		st->strm.next_out = st->outbuffer;
		st->readoffset = 0;
		if (!st->strm.avail_in)
		{
			qofs_t sz;
			sz = st->cend - st->cofs;
			if (sz > sizeof(st->inbuffer))
				sz = sizeof(st->inbuffer);
			if (sz)
			{
				//feed it.
				if (Sys_LockMutex(st->source->mutex))
				{
					VFS_SEEK(st->source->raw, st->cofs);
					st->strm.avail_in = VFS_READ(st->source->raw, st->inbuffer, sz);
					Sys_UnlockMutex(st->source->mutex);
				}
				else
					st->strm.avail_in = 0;
				st->strm.next_in = st->inbuffer;
				st->cofs += st->strm.avail_in;
#ifdef ZIPCRYPT
				if (st->encrypted)
					FSZIP_DecryptBlock(st, st->inbuffer, st->strm.avail_in);
#endif
			}
			if (!st->strm.avail_in)
				eof = true;
		}
		err = qinflate(&st->strm,Z_SYNC_FLUSH);
		if (err == Z_STREAM_END)
			eof = true;
		else if (err != Z_OK)
			break;
	}
	return read;
}

static void FSZIP_Decompress_Destroy(struct decompressstate *st)
{
	qinflateEnd(&st->strm);
	Z_Free(st);
}
static vfsfile_t *FSZIP_Decompress_ToTempFile(struct decompressstate *decompress)
{	//if they're going to seek on a file in a zip, let's just copy it out
	qofs_t cstart = decompress->cstart, csize = decompress->cend - cstart;
	qofs_t upos = 0, usize = decompress->usize;
	qofs_t chunk;
	struct decompressstate *nc;
	qbyte buffer[16384];
	vfsfile_t *defer;
	zipfile_t *source = decompress->source;
	qboolean encrypted = decompress->encrypted;
	unsigned int cryptkeys[3];
	const z_crc_t *crctab = decompress->crctable;

	memcpy(cryptkeys, decompress->initialkey, sizeof(cryptkeys));

	defer = FS_OpenTemp();
	if (defer)
	{
		FSZIP_Decompress_Destroy(decompress);
		decompress = NULL;

		nc = FSZIP_Decompress_Init(source, cstart, csize, usize, NULL, NULL, 0);
		nc->encrypted = encrypted;
		nc->crctable = crctab;
		memcpy(nc->initialkey, cryptkeys, sizeof(nc->initialkey));
		memcpy(nc->cryptkey, cryptkeys, sizeof(nc->cryptkey));

		while (upos < usize)
		{
			chunk = usize - upos;
			if (chunk > sizeof(buffer))
				chunk = sizeof(buffer);
			if (!FSZIP_Decompress_Read(nc, buffer, chunk))
				break;
			if (VFS_WRITE(defer, buffer, chunk) != chunk)
				break;
			upos += chunk;
		}
		FSZIP_Decompress_Destroy(nc);

		return defer;
	}
	return NULL;
}
#else
struct decompressstate
{
//	zipfile_t *source;
//	qofs_t cstart;	//position of start of data
//	qofs_t cofs;	//current read offset
//	qofs_t cend;	//compressed data ends at this point.
//	qofs_t usize;	//data remaining. refuse to read more bytes than this.
//	unsigned char inbuffer[16384];
//	unsigned char outbuffer[16384];
//	unsigned int readoffset;
};
struct decompressstate *FSZIP_Decompress_Init(zipfile_t *source, qofs_t start, qofs_t csize, qofs_t usize, char *filename, char *password, unsigned int crc)
{
	return NULL;
}

qofs_t FSZIP_Decompress_Read(struct decompressstate *st, qbyte *buffer, qofs_t bytes)
{
	return 0;
}

void FSZIP_Decompress_Destroy(struct decompressstate *st)
{
}

vfsfile_t *FSZIP_Decompress_ToTempFile(struct decompressstate *decompress)
{
	return NULL;
}
#endif

//an open vfsfile_t
typedef struct {
	vfsfile_t funcs;

	vfsfile_t *defer;	//if set, reads+seeks are defered to this file instead.

	//in case we're forced away.
	zipfile_t *parent;
	struct decompressstate *decompress;
	qofs_t pos;
	qofs_t length;	//try and optimise some things
//	int index;
	qofs_t startpos;	//file data offset
} vfszip_t;

static int QDECL VFSZIP_ReadBytes (struct vfsfile_s *file, void *buffer, int bytestoread)
{
	int read;
	vfszip_t *vfsz = (vfszip_t*)file;

	if (vfsz->defer)
		return VFS_READ(vfsz->defer, buffer, bytestoread);

	if (vfsz->decompress)
	{
		read = FSZIP_Decompress_Read(vfsz->decompress, buffer, bytestoread);
	}
	else if (Sys_LockMutex(vfsz->parent->mutex))
	{
		VFS_SEEK(vfsz->parent->raw, vfsz->pos+vfsz->startpos);
		if (vfsz->pos + bytestoread > vfsz->length)
			bytestoread = max(0, vfsz->length - vfsz->pos);
		read = VFS_READ(vfsz->parent->raw, buffer, bytestoread);
		Sys_UnlockMutex(vfsz->parent->mutex);
	}
	else
		read = 0;

	if (read < bytestoread)
		((char*)buffer)[read] = 0;

	vfsz->pos += read;
	return read;
}
static qboolean QDECL VFSZIP_Seek (struct vfsfile_s *file, qofs_t pos)
{
	vfszip_t *vfsz = (vfszip_t*)file;

	if (vfsz->defer)
		return VFS_SEEK(vfsz->defer, pos);

	//This is *really* inefficient
	if (vfsz->decompress)
	{	//if they're going to seek on a file in a zip, let's just copy it out
		vfsz->defer = FSZIP_Decompress_ToTempFile(vfsz->decompress);
		vfsz->decompress = NULL;
		if (vfsz->defer)
			return VFS_SEEK(vfsz->defer, pos);
		return false;
	}

	if (pos < 0 || pos > vfsz->length)
		return false;
	vfsz->pos = pos;

	return true;
}
static qofs_t QDECL VFSZIP_Tell (struct vfsfile_s *file)
{
	vfszip_t *vfsz = (vfszip_t*)file;

	if (vfsz->defer)
		return VFS_TELL(vfsz->defer);

	return vfsz->pos;
}
static qofs_t QDECL VFSZIP_GetLen (struct vfsfile_s *file)
{
	vfszip_t *vfsz = (vfszip_t*)file;
	return vfsz->length;
}
static qboolean QDECL VFSZIP_Close (struct vfsfile_s *file)
{
	vfszip_t *vfsz = (vfszip_t*)file;

	if (vfsz->defer)
		VFS_CLOSE(vfsz->defer);

	if (vfsz->decompress)
		FSZIP_Decompress_Destroy(vfsz->decompress);

	FSZIP_ClosePath(&vfsz->parent->pub);
	Z_Free(vfsz);

	return true;	//FIXME: return an error if the crc was wrong.
}

static qboolean FSZIP_ValidateLocalHeader(zipfile_t *zip, zpackfile_t *zfile, qofs_t *datastart, qofs_t *datasize);

static vfsfile_t *QDECL FSZIP_OpenVFS(searchpathfuncs_t *handle, flocation_t *loc, const char *mode)
{
	zipfile_t *zip = (void*)handle;
	vfszip_t *vfsz;
	unsigned int flags;
	qofs_t datasize = 0;

	if (strcmp(mode, "rb"))
		return NULL; //urm, unable to write/append

	if (qofs_Error(loc->len))
		return NULL;

	flags = zip->files[loc->index].flags;
	if (flags & ZFL_CORRUPT)
		return NULL;

	vfsz = Z_Malloc(sizeof(vfszip_t));

	vfsz->parent = zip;
	vfsz->length = loc->len;

#ifdef _DEBUG
	Q_strncpyz(vfsz->funcs.dbgname, zip->files[loc->index].name, sizeof(vfsz->funcs.dbgname));
#endif
	vfsz->funcs.Close = VFSZIP_Close;
	vfsz->funcs.GetLen = VFSZIP_GetLen;
	vfsz->funcs.ReadBytes = VFSZIP_ReadBytes;
	//vfsz->funcs.WriteBytes = VFSZIP_WriteBytes;
	vfsz->funcs.Seek = VFSZIP_Seek;
	vfsz->funcs.Tell = VFSZIP_Tell;
	vfsz->funcs.WriteBytes = NULL;
	vfsz->funcs.seekingisabadplan = true;

	if (!FSZIP_ValidateLocalHeader(zip, &zip->files[loc->index], &vfsz->startpos, &datasize))
	{
		Z_Free(vfsz);
		return NULL;
	}

	if (flags & ZFL_DEFLATED)
	{
#ifdef ZIPCRYPT
		//FIXME: Cvar_Get is not threadsafe.
		char *password = (flags & ZFL_WEAKENCRYPT)?Cvar_Get("fs_zip_password", "thisispublic", 0, "Filesystem")->string:NULL;
#else
		char *password = NULL;
#endif
		vfsz->decompress = FSZIP_Decompress_Init(zip, vfsz->startpos, datasize, vfsz->length, zip->files[loc->index].name, password, zip->files[loc->index].crc);
		if (!vfsz->decompress)
		{
			/*
			windows explorer tends to use deflate64 on large files, which zlib and thus we, do not support, thus this is a 'common' failure path
			this might also trigger from other errors, of course.
			*/
			Z_Free(vfsz);
			return NULL;
		}
	}

	if (Sys_LockMutex(zip->mutex))
	{
		zip->references++;
		Sys_UnlockMutex(zip->mutex);
	}
	else
	{
		if (vfsz->decompress)
			FSZIP_Decompress_Destroy(vfsz->decompress);
		Z_Free(vfsz);
		return NULL;
	}

	return (vfsfile_t*)vfsz;
}


//ZIP features:
//zip64 for huge zips
//utf-8 encoding support (non-utf-8 is always ibm437)
//symlink flag
//compression mode: store
//compression mode: deflate (via zlib)
//bigendian cpus. everything misaligned.

//NOT supported:
//compression mode: deflate64
//other compression modes
//split archives
//if central dir is crypted/compressed, the archive will fail to open
//if a file is crypted/compressed, the file will (internally) be marked as corrupt
//crc verification
//infozip utf-8 name override.
//other 'extra' fields.

struct zipinfo
{
	unsigned int	thisdisk;					//this disk number
	unsigned int	centraldir_startdisk;		//central directory starts on this disk
	qofs_t			centraldir_numfiles_disk;	//number of files in the centraldir on this disk
	qofs_t			centraldir_numfiles_all;	//number of files in the centraldir on all disks
	qofs_t			centraldir_size;			//total size of the centraldir
	qofs_t			centraldir_offset;			//start offset of the centraldir with respect to the first disk that contains it
	unsigned short	commentlength;				//length of the comment at the end of the archive.

	unsigned int	zip64_centraldirend_disk;	//zip64 weirdness
	qofs_t			zip64_centraldirend_offset;
	unsigned int	zip64_diskcount;
	qofs_t			zip64_eocdsize;
	unsigned short	zip64_version_madeby;
	unsigned short	zip64_version_needed;

	unsigned short	centraldir_compressionmethod;
	unsigned short	centraldir_algid;

	qofs_t			centraldir_end;
	qofs_t			zipoffset;
};
struct zipcentralentry
{
	unsigned char	*fname;
	qofs_t			cesize;
	unsigned int	flags;
	time_t			mtime;

	//PK12
	unsigned short	version_madeby;
	unsigned short	version_needed;
	unsigned short	gflags;
	unsigned short	cmethod;
	unsigned short	lastmodfiletime;
	unsigned short	lastmodfiledate;
	unsigned int	crc32;
	qofs_t			csize;
	qofs_t			usize;
	unsigned short	fnane_len;
	unsigned short	extra_len;
	unsigned short	comment_len;
	unsigned int	disknum;
	unsigned short	iattributes;
	unsigned int	eattributes;
	qofs_t	localheaderoffset;	//from start of disk
};

struct ziplocalentry
{
      //PK34
	unsigned short	version_needed;
	unsigned short	gpflags;
	unsigned short	cmethod;
	unsigned short	lastmodfiletime;
	unsigned short	lastmodfiledate;
	unsigned int	crc32;
	qofs_t			csize;
	qofs_t			usize;
	unsigned short	fname_len;
	unsigned short	extra_len;
};

#define LittleU2FromPtr(p) (unsigned int)((p)[0] | ((p)[1]<<8u))
#define LittleU4FromPtr(p) (unsigned int)((p)[0] | ((p)[1]<<8u) | ((p)[2]<<16u) | ((p)[3]<<24u))
#define LittleU8FromPtr(p) qofs_Make(LittleU4FromPtr(p), LittleU4FromPtr((p)+4))

#define SIZE_LOCALENTRY 30
#define SIZE_ENDOFCENTRALDIRECTORY 22
#define SIZE_ZIP64ENDOFCENTRALDIRECTORY_V1 56
#define SIZE_ZIP64ENDOFCENTRALDIRECTORY_V2 84
#define SIZE_ZIP64ENDOFCENTRALDIRECTORY SIZE_ZIP64ENDOFCENTRALDIRECTORY_V2
#define SIZE_ZIP64ENDOFCENTRALDIRECTORYLOCATOR 20

//check the local header and determine the place the data actually starts.
//we don't do much checking, just validating a few fields that are copies of fields we tracked elsewhere anyway.
//don't bother with the crc either
static qboolean FSZIP_ValidateLocalHeader(zipfile_t *zip, zpackfile_t *zfile, qofs_t *datastart, qofs_t *datasize)
{
	struct ziplocalentry local;
	qbyte localdata[SIZE_LOCALENTRY];
	qofs_t localstart = zfile->localpos;

	
	if (!Sys_LockMutex(zip->mutex))
		return false;	//ohnoes
	VFS_SEEK(zip->raw, localstart);
	VFS_READ(zip->raw, localdata, sizeof(localdata));
	Sys_UnlockMutex(zip->mutex);

	//make sure we found the right sort of table.
	if (localdata[0] != 'P' ||
		localdata[1] != 'K' ||
		localdata[2] != 3 ||
		localdata[3] != 4)
		return false;

	local.version_needed = LittleU2FromPtr(localdata+4);
	local.gpflags = LittleU2FromPtr(localdata+6);
	local.cmethod = LittleU2FromPtr(localdata+8);
	local.lastmodfiletime = LittleU2FromPtr(localdata+10);
	local.lastmodfiledate = LittleU2FromPtr(localdata+12);
	local.crc32 = LittleU4FromPtr(localdata+14);
	local.csize = LittleU4FromPtr(localdata+18);
	local.usize = LittleU4FromPtr(localdata+22);
	local.fname_len = LittleU2FromPtr(localdata+26);
	local.extra_len = LittleU2FromPtr(localdata+28);

	localstart += SIZE_LOCALENTRY;
	localstart += local.fname_len;

	//parse extra
	if (local.usize == 0xffffffffu || local.csize == 0xffffffffu)	//don't bother otherwise.
	if (local.extra_len)
	{
		qbyte extradata[65536];
		qbyte *extra = extradata;
		qbyte *extraend = extradata + local.extra_len;
		unsigned short extrachunk_tag;
		unsigned short extrachunk_len;

		if (!Sys_LockMutex(zip->mutex))
			return false;	//ohnoes
		VFS_SEEK(zip->raw, localstart);
		VFS_READ(zip->raw, extradata, sizeof(extradata));
		Sys_UnlockMutex(zip->mutex);


		while(extra+4 < extraend)
		{
			extrachunk_tag = LittleU2FromPtr(extra+0);
			extrachunk_len = LittleU2FromPtr(extra+2);
			if (extra + extrachunk_len > extraend)
				break;	//error
			extra += 4;

			switch(extrachunk_tag)
			{
			case 1:	//zip64 extended information extra field. the attributes are only present if the reegular file info is nulled out with a -1
				if (local.usize == 0xffffffffu)
				{
					local.usize = LittleU8FromPtr(extra);
					extra += 8;
				}
				if (local.csize == 0xffffffffu)
				{
					local.csize = LittleU8FromPtr(extra);
					extra += 8;
				}
				break;
			default:
/*				Con_Printf("Unknown chunk %x\n", extrachunk_tag);
			case 0x000a:	//NTFS (timestamps)
			case 0x5455:	//extended timestamp
			case 0x7875:	//unix uid/gid
*/				extra += extrachunk_len;
				break;
			}
		}
	}
	localstart += local.extra_len;
	*datastart = localstart;	//this is the end of the local block, and the start of the data block (well, actually, should be encryption, but we don't support that).
	*datasize = local.csize;

	if (local.gpflags & (1u<<3))
	{
		//crc, usize, and csize were not known at the time the file was compressed.
		//there is a 'data descriptor' after the file data, but to parse that we would need to decompress the file.
		//instead, just depend upon upon the central directory and don't bother checking.
	}
	else
	{
		if (local.crc32 != zfile->crc)
			return false;
		if (local.usize != zfile->filelen)
			return false;
	}

	//FIXME: with pure paths, we still don't bother checking the crc (again, would require decompressing the entire file in advance).

	if (local.cmethod == 0)
		return (zfile->flags & (ZFL_STORED|ZFL_CORRUPT|ZFL_DEFLATED)) == ZFL_STORED;
	if (local.cmethod == 8)
		return (zfile->flags & (ZFL_STORED|ZFL_CORRUPT|ZFL_DEFLATED)) == ZFL_DEFLATED;
	return false;	//some other method that we don't know.
}

static qboolean FSZIP_ReadCentralEntry(zipfile_t *zip, qbyte *data, struct zipcentralentry *entry)
{
	entry->flags = 0;
	entry->fname = "";
	entry->fnane_len = 0;

	if (data[0] != 'P' ||
		data[1] != 'K' ||
		data[2] != 1 ||
		data[3] != 2)
		return false;	//verify the signature

	//if we read too much, we'll catch it after.
	entry->version_madeby = LittleU2FromPtr(data+4);
	entry->version_needed = LittleU2FromPtr(data+6);
	entry->gflags = LittleU2FromPtr(data+8);
	entry->cmethod = LittleU2FromPtr(data+10);
	entry->lastmodfiletime = LittleU2FromPtr(data+12);
	entry->lastmodfiledate = LittleU2FromPtr(data+12);
	entry->crc32 = LittleU4FromPtr(data+16);
	entry->csize = LittleU4FromPtr(data+20);
	entry->usize = LittleU4FromPtr(data+24);
	entry->fnane_len = LittleU2FromPtr(data+28);
	entry->extra_len = LittleU2FromPtr(data+30);
	entry->comment_len = LittleU2FromPtr(data+32);
	entry->disknum = LittleU2FromPtr(data+34);
	entry->iattributes = LittleU2FromPtr(data+36);
	entry->eattributes = LittleU4FromPtr(data+38);
	entry->localheaderoffset = LittleU4FromPtr(data+42);
	entry->cesize = 46;

	//mark the filename position
	entry->fname = data+entry->cesize;
	entry->cesize += entry->fnane_len;

	entry->mtime = 0;

	//parse extra
	if (entry->extra_len)
	{
		qbyte *extra = data + entry->cesize;
		qbyte *extraend = extra + entry->extra_len;
		unsigned short extrachunk_tag;
		unsigned short extrachunk_len;
		while(extra+4 < extraend)
		{
			extrachunk_tag = LittleU2FromPtr(extra+0);
			extrachunk_len = LittleU2FromPtr(extra+2);
			if (extra + extrachunk_len > extraend)
				break;	//error
			extra += 4;

			switch(extrachunk_tag)
			{
			case 1:	//zip64 extended information extra field. the attributes are only present if the reegular file info is nulled out with a -1
				if (entry->usize == 0xffffffffu)
				{
					entry->usize = LittleU8FromPtr(extra);
					extra += 8;
				}
				if (entry->csize == 0xffffffffu)
				{
					entry->csize = LittleU8FromPtr(extra);
					extra += 8;
				}
				if (entry->localheaderoffset == 0xffffffffu)
				{
					entry->localheaderoffset = LittleU8FromPtr(extra);
					extra += 8;
				}
				if (entry->disknum == 0xffffu)
				{
					entry->disknum = LittleU4FromPtr(extra);
					extra += 4;
				}
				break;
			case 0x000a:	//NTFS extra field
				//0+4: reserved
				//4+2: subtag(must be 1, for times)
				//6+2: subtagsize(times: must be == 8*3+4)
				//8+8: mtime
				//16+8: atime
				//24+8: ctime
				if (extrachunk_len >= 32 && LittleU2FromPtr(extra+4) == 1 && LittleU2FromPtr(extra+6) == 8*3)	
					entry->mtime = LittleU8FromPtr(extra+8) / 10000000ULL - 11644473600ULL;
				else
					Con_Printf("zip: unsupported ntfs subchunk %x\n", extrachunk_tag);
				extra += extrachunk_len;
				break;
			case 0x5455:
				if (extra[0] & 1)
					entry->mtime = LittleU4FromPtr(extra+1);
				//access and creation do NOT exist in the central header.
				extra += extrachunk_len;
				break;
			default:
/*				Con_Printf("Unknown chunk %x\n", extrachunk_tag);
			case 0x5455:	//extended timestamp
			case 0x7875:	//unix uid/gid
			case 0x9901:	//aes crypto
*/				extra += extrachunk_len;
				break;
			}
		}
		entry->cesize += entry->extra_len;
	}

	//parse comment
	entry->cesize += entry->comment_len;

	//check symlink flags
	{
		qbyte madeby = entry->version_madeby>>8;	//system
		//vms, unix, or beos file attributes includes a symlink attribute.
		//symlinks mean the file contents is just the name of another file.
		if (madeby == 2 || madeby == 3 || madeby == 16)
		{
			unsigned short unixattr = entry->eattributes>>16;
			if ((unixattr & 0xF000) == 0xA000)//fa&S_IFMT==S_IFLNK 
				entry->flags |= ZFL_SYMLINK;
			else if ((unixattr & 0xA000) == 0xA000)//fa&S_IFMT==S_IFLNK 
				entry->flags |= ZFL_SYMLINK;
		}
	}

	if (entry->gflags & (1u<<0))	//encrypted
	{
#ifdef ZIPCRYPT
		entry->flags |= ZFL_WEAKENCRYPT;
#else
		entry->flags |= ZFL_CORRUPT;
#endif
	}


	if (entry->gflags & (1u<<5))	//is patch data
		entry->flags |= ZFL_CORRUPT;
	else if (entry->gflags & (1u<<6))	//strong encryption
		entry->flags |= ZFL_CORRUPT;
	else if (entry->gflags & (1u<<13))	//strong encryption
		entry->flags |= ZFL_CORRUPT;
	else if (entry->cmethod == 0)
		entry->flags |= ZFL_STORED;
	//1: shrink
	//2-5: reduce
	//6: implode
	//7: tokenize
	else if (entry->cmethod == 8)
		entry->flags |= ZFL_DEFLATED;
	//8: deflate64 - patented. sometimes written by microsoft's crap, so this might be problematic. only minor improvements.
	//10: implode
	//12: bzip2
//	else if (entry->cmethod == 12)
//		entry->flags |= ZFL_BZIP2;
//	else if (entry->cmethod == 14)
//		entry->flags |= ZFL_LZMA;
	//19: lz77
	//97: wavpack
	//98: ppmd
	else 
		entry->flags |= ZFL_CORRUPT;	//unsupported compression method.

	if ((entry->flags & ZFL_WEAKENCRYPT) && !(entry->flags & ZFL_DEFLATED))
		entry->flags |= ZFL_CORRUPT;	//only support decryption with deflate.
	return true;
}

//for compatibility with windows, this is an IBM437 -> unicode translation (ucs-2 is sufficient here)
//the zip codepage is defined as either IBM437(aka: dos) or UTF-8. Systems that use a different codepage are buggy.
unsigned short ibmtounicode[256] =
{
	0x0000,0x263A,0x263B,0x2665,0x2666,0x2663,0x2660,0x2022,0x25D8,0x25CB,0x25D9,0x2642,0x2640,0x266A,0x266B,0x263C,	//0x00(non-ascii, display-only)
	0x25BA,0x25C4,0x2195,0x203C,0x00B6,0x00A7,0x25AC,0x21A8,0x2191,0x2193,0x2192,0x2190,0x221F,0x2194,0x25B2,0x25BC,	//0x10(non-ascii, display-only)
	0x0020,0x0021,0x0022,0x0023,0x0024,0x0025,0x0026,0x0027,0x0028,0x0029,0x002A,0x002B,0x002C,0x002D,0x002E,0x002F,	//0x20(ascii)
	0x0030,0x0031,0x0032,0x0033,0x0034,0x0035,0x0036,0x0037,0x0038,0x0039,0x003A,0x003B,0x003C,0x003D,0x003E,0x003F,	//0x30(ascii)
	0x0040,0x0041,0x0042,0x0043,0x0044,0x0045,0x0046,0x0047,0x0048,0x0049,0x004A,0x004B,0x004C,0x004D,0x004E,0x004F,	//0x40(ascii)
	0x0050,0x0051,0x0052,0x0053,0x0054,0x0055,0x0056,0x0057,0x0058,0x0059,0x005A,0x005B,0x005C,0x005D,0x005E,0x005F,	//0x50(ascii)
	0x0060,0x0061,0x0062,0x0063,0x0064,0x0065,0x0066,0x0067,0x0068,0x0069,0x006A,0x006B,0x006C,0x006D,0x006E,0x006F,	//0x60(ascii)
	0x0070,0x0071,0x0072,0x0073,0x0074,0x0075,0x0076,0x0077,0x0078,0x0079,0x007A,0x007B,0x007C,0x007D,0x007E,0x2302,	//0x70(mostly ascii, one display-only)
	0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7,0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5,	//0x80(non-ascii, printable)
	0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9,0x00FF,0x00D6,0x00DC,0x00A2,0x00A3,0x00A5,0x20A7,0x0192,	//0x90(non-ascii, printable)
	0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA,0x00BF,0x2310,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB,	//0xa0(non-ascii, printable)
	0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,	//0xb0(box-drawing, printable)
	0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,	//0xc0(box-drawing, printable)
	0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580,	//0xd0(box-drawing, printable)
	0x03B1,0x00DF,0x0393,0x03C0,0x03A3,0x03C3,0x00B5,0x03C4,0x03A6,0x0398,0x03A9,0x03B4,0x221E,0x03C6,0x03B5,0x2229,	//0xe0(maths(greek), printable)
	0x2261,0x00B1,0x2265,0x2264,0x2320,0x2321,0x00F7,0x2248,0x00B0,0x2219,0x00B7,0x221A,0x207F,0x00B2,0x25A0,0x00A0,	//0xf0(maths, printable)
};

static qboolean FSZIP_EnumerateCentralDirectory(zipfile_t *zip, struct zipinfo *info)
{
	qboolean success = false;
	zpackfile_t		*f;
	struct zipcentralentry entry;
	qofs_t ofs = 0;
	unsigned int i;
	//lazily read the entire thing.
	qbyte *centraldir = BZ_Malloc(info->centraldir_size);
	if (centraldir)
	{
		VFS_SEEK(zip->raw, info->centraldir_offset+info->zipoffset);
		if (VFS_READ(zip->raw, centraldir, info->centraldir_size) == info->centraldir_size)
		{
			zip->numfiles = info->centraldir_numfiles_disk;
			zip->files = f = Z_Malloc (zip->numfiles * sizeof(zpackfile_t));

			for (i = 0; i < zip->numfiles; i++)
			{
				if (!FSZIP_ReadCentralEntry(zip, centraldir+ofs, &entry) || ofs + entry.cesize > info->centraldir_size)
					break;

				f->crc = entry.crc32;

				//copy out the filename and lowercase it
				if (entry.gflags & (1u<<11))
				{	//unicode encoding
					if (entry.fnane_len > sizeof(f->name)-1)
						entry.fnane_len = sizeof(f->name)-1;
					memcpy(f->name, entry.fname, entry.fnane_len);
					f->name[entry.fnane_len] = 0;
				}
				else
				{
					int i;
					int nlen = 0;
					int cl;
					for (i = 0; i < entry.fnane_len; i++)
					{
						cl = utf8_encode(f->name+nlen, ibmtounicode[entry.fname[i]], sizeof(f->name)-1 - nlen);
						if (!cl)	//overflowed, truncate cleanly.
							break;
						nlen += cl;
					}
					f->name[nlen] = 0;

				}
				Q_strlwr(f->name);

				f->filelen = entry.usize;
				f->localpos = entry.localheaderoffset+info->zipoffset;
				f->flags = entry.flags;
				f->mtime = entry.mtime;

				ofs += entry.cesize;
				f++;
			}

			success = i == zip->numfiles;
			if (!success)
			{
				Z_Free(zip->files);
				zip->files = NULL;
				zip->numfiles = 0;
			}
		}
	}

	BZ_Free(centraldir);
	return success;
}

static qboolean FSZIP_FindEndCentralDirectory(zipfile_t *zip, struct zipinfo *info)
{
	qboolean result = false;
	//zip comment is capped to 65k or so, so we can use a single buffer for this
	qbyte traildata[0x10000 + SIZE_ENDOFCENTRALDIRECTORY+SIZE_ZIP64ENDOFCENTRALDIRECTORYLOCATOR];
	qbyte *magic;
	unsigned int trailsize = 0x10000 + SIZE_ENDOFCENTRALDIRECTORY+SIZE_ZIP64ENDOFCENTRALDIRECTORYLOCATOR;
	if (trailsize > zip->rawsize)
		trailsize = zip->rawsize;
	//FIXME: do in a loop to avoid a huge block of stack use
	VFS_SEEK(zip->raw, zip->rawsize - trailsize);
	VFS_READ(zip->raw, traildata, trailsize);

	memset(info, 0, sizeof(*info));

	for (magic = traildata+trailsize-SIZE_ENDOFCENTRALDIRECTORY; magic >= traildata; magic--)
	{
		if (magic[0] == 'P' && 
			magic[1] == 'K' && 
			magic[2] == 5 && 
			magic[3] == 6)
		{
			info->centraldir_end = (zip->rawsize-trailsize)+(magic-traildata);

			info->thisdisk					= LittleU2FromPtr(magic+4);
			info->centraldir_startdisk		= LittleU2FromPtr(magic+6);
			info->centraldir_numfiles_disk	= LittleU2FromPtr(magic+8);
			info->centraldir_numfiles_all	= LittleU2FromPtr(magic+10);
			info->centraldir_size			= LittleU4FromPtr(magic+12);
			info->centraldir_offset			= LittleU4FromPtr(magic+16);
			info->commentlength				= LittleU2FromPtr(magic+20);

			result = true;
			break;
		}
	}

	if (!result)
		Con_Printf("zip: unable to find end-of-central-directory\n");
	else

	//now look for a zip64 header.
	//this gives more larger archives, more files, larger files, more spanned disks.
	//note that the central directory itself is the same, it just has a couple of extra attributes on files that need them.
	for (magic -= SIZE_ZIP64ENDOFCENTRALDIRECTORYLOCATOR; magic >= traildata; magic--)
	{
		if (magic[0] == 'P' && 
			magic[1] == 'K' && 
			magic[2] == 6 && 
			magic[3] == 7)
		{
			qbyte z64eocd[SIZE_ZIP64ENDOFCENTRALDIRECTORY];

			info->zip64_centraldirend_disk		= LittleU4FromPtr(magic+4);
			info->zip64_centraldirend_offset	= LittleU8FromPtr(magic+8);
			info->zip64_diskcount				= LittleU4FromPtr(magic+16);

			if (info->zip64_diskcount != 1 || info->zip64_centraldirend_disk != 0)
			{
				Con_Printf("zip: archive is spanned\n");
				return false;
			}

			VFS_SEEK(zip->raw, info->zip64_centraldirend_offset);
			VFS_READ(zip->raw, z64eocd, sizeof(z64eocd));

			if (z64eocd[0] == 'P' &&
				z64eocd[1] == 'K' &&
				z64eocd[2] == 6 &&
				z64eocd[3] == 6)
			{
				info->zip64_eocdsize						= LittleU8FromPtr(z64eocd+4) + 12;
				info->zip64_version_madeby					= LittleU2FromPtr(z64eocd+12);
				info->zip64_version_needed       			= LittleU2FromPtr(z64eocd+14);
				info->thisdisk             					= LittleU4FromPtr(z64eocd+16);
				info->centraldir_startdisk					= LittleU4FromPtr(z64eocd+20);
				info->centraldir_numfiles_disk  			= LittleU8FromPtr(z64eocd+24);
				info->centraldir_numfiles_all				= LittleU8FromPtr(z64eocd+32);
				info->centraldir_size   					= LittleU8FromPtr(z64eocd+40);
				info->centraldir_offset						= LittleU8FromPtr(z64eocd+48);

				if (info->zip64_eocdsize >= 84)
				{
					info->centraldir_compressionmethod		= LittleU2FromPtr(z64eocd+56);
//					info->zip64_2_centraldir_csize			= LittleU8FromPtr(z64eocd+58);
//					info->zip64_2_centraldir_usize			= LittleU8FromPtr(z64eocd+66);
					info->centraldir_algid					= LittleU2FromPtr(z64eocd+74);
//					info.zip64_2_bitlen						= LittleU2FromPtr(z64eocd+76);
//					info->zip64_2_flags						= LittleU2FromPtr(z64eocd+78);
//					info->zip64_2_hashid					= LittleU2FromPtr(z64eocd+80);
//					info->zip64_2_hashlength				= LittleU2FromPtr(z64eocd+82);
					//info->zip64_2_hashdata				= LittleUXFromPtr(z64eocd+84, info->zip64_2_hashlength);
				}
			}
			else
			{
				Con_Printf("zip: zip64 end-of-central directory at unknown offset.\n");
				result = false;
			}
     
			break;
		}
	}

	if (info->thisdisk || info->centraldir_startdisk || info->centraldir_numfiles_disk != info->centraldir_numfiles_all)
	{
		Con_Printf("zip: archive is spanned\n");
		result = false;
	}
	if (info->centraldir_compressionmethod || info->centraldir_algid)
	{
		Con_Printf("zip: encrypted centraldir\n");
		result = false;
	}

	return result;
}

/*
=================
COM_LoadZipFile

Takes an explicit (not game tree related) path to a pak file.

Loads the header and directory, adding the files at the beginning
of the list so they override previous pack files.
=================
*/
searchpathfuncs_t *QDECL FSZIP_LoadArchive (vfsfile_t *packhandle, const char *desc)
{
	zipfile_t *zip;
	struct zipinfo info;

	if (!packhandle)
		return NULL;

	zip = Z_Malloc(sizeof(zipfile_t));
	Q_strncpyz(zip->filename, desc, sizeof(zip->filename));
	zip->raw = packhandle;
	zip->rawsize = VFS_GETLEN(zip->raw);

	//find the footer
	if (!FSZIP_FindEndCentralDirectory(zip, &info))
	{
		Z_Free(zip);
		Con_TPrintf ("Failed opening zipfile \"%s\" corrupt?\n", desc);
		return NULL;
	}

	//find the central directory.
	if (!FSZIP_FindEndCentralDirectory(zip, &info))
	{
		Z_Free(zip);
		Con_TPrintf ("Failed opening zipfile \"%s\" corrupt?\n", desc);
		return NULL;
	}

	//now read it.
	if (!FSZIP_EnumerateCentralDirectory(zip, &info) && !info.zip64_diskcount)
	{
		//uh oh... the central directory wasn't where it was meant to be!
		//assuming that the endofcentraldir is packed at the true end of the centraldir (and that we're not zip64 and thus don't have an extra block), then we can guess based upon the offset difference
		info.zipoffset = info.centraldir_end - (info.centraldir_offset+info.centraldir_size);
		if (FSZIP_EnumerateCentralDirectory(zip, &info))
		{
			Z_Free(zip);
			Con_TPrintf ("zipfile \"%s\" appears to be missing its central directory\n", desc);
			return NULL;
		}
	}

	zip->references = 1;
	zip->mutex = Sys_CreateMutex();

//	Con_TPrintf ("Added zipfile %s (%i files)\n", desc, zip->numfiles);

	zip->pub.fsver				= FSVER;
	zip->pub.GetPathDetails		= FSZIP_GetPathDetails;
	zip->pub.ClosePath			= FSZIP_ClosePath;
	zip->pub.BuildHash			= FSZIP_BuildHash;
	zip->pub.FindFile			= FSZIP_FLocate;
	zip->pub.ReadFile			= FSZIP_ReadFile;
	zip->pub.EnumerateFiles		= FSZIP_EnumerateFiles;
	zip->pub.GeneratePureCRC	= FSZIP_GeneratePureCRC;
	zip->pub.OpenVFS			= FSZIP_OpenVFS;
	return &zip->pub;
}





#if 0
//our download protocol permits requesting various different parts of the file.
//this means that its theoretically possible to download sections pk3s such that we can skip blocks that contain files that we already have.

typedef struct
{
	struct archivedeltafuncs_s
	{
		//called when the downloader needs to know a new target region. each call should return a new region, eventually returning the entire file as data is injected from elsewhere
		//return false on error.
		qboolean (*GetNextRange) (struct archivedeltafuncs_s *archive, qofs_t *start, qofs_t *end);

		//called when the download completes/aborts. frees memory.
		void (*Finish) (struct archivedeltafuncs_s);
	} pub;

	qdownload_t *dl;

	enum
	{
		step_eocd,
		step_cd,
		step_file
	} step;

	struct
	{
		qofs_t start;
		qofs_t end;
	} eocd;

	zipfile_t *old;

	zipfile_t zip;
	struct zipinfo info;
} ziparchivedelta_t;

void FSZIPDL_Finish(struct archivedeltafuncs_s *archive)
{
	ziparchivedelta_t *za = (ziparchivedelta_t*)archive;

	za->old->pub.ClosePath(&za->old->pub);

	if (za->zip.files)
		Z_Free(za->zip.files);

	Z_Free(za);
}
qboolean FSZIPDL_GetNextRange(struct archivedeltafuncs_s *archive, qofs_t *start, qofs_t *end)
{
	flocation_t loc;
	int i;
	ziparchivedelta_t *za = (ziparchivedelta_t*)archive;
	switch(za->step)
	{
	case step_eocd:
		//find the header (actually, the trailer)
		*start = za->eocd.start;
		*end = za->eocd.end;
		za->step++;
		break;
	case step_cd:
		if (!FSZIP_FindEndCentralDirectory(&za->zip, &za->info))
			return false;	//corrupt/unusable.

		//central directory should be located at this position
		*start = za->info.centraldir_offset+za->info.zipoffset;
		*end = za->info.centraldir_end+za->info.zipoffset;
		za->step++;
		break;
	case step_file:
		if (FSZIP_EnumerateCentralDirectory(&za->zip, &za->info))
		{
			return false;	//couldn't find the central directory properly (fixme: this can happen with zip32 self-extractors)
		}

		//walk through the central directory looking for matching files in the existing versions of the zip
		for (i = 0; i < za->zip.numfiles; i++)
			if (FSZIP_FLocate(&za->old->pub, &loc, za->zip.files[i].name, NULL) != FF_NOTFOUND)
			{
				if (za->old->files[loc.index].crc == za->zip.files[i].crc)
				{
					qofs_t datastart, datasize;
					if (FSZIP_ValidateLocalHeader(za->old, &za->old->files[loc.index], &datastart, &datasize))
					{
						size_t chunk;
						datastart = za->old->files[loc.index].localpos;

						//tell the download context that we already know this data region
						DL_Completed(za->dl, datastart, datastart + datasize);
						//and inject the data into the downloaded file.
						VFS_SEEK(za->old->raw, datastart);
						VFS_SEEK(za->dl->file, za->zip.files[i].localpos);
						for(;datasize;)
						{
							char copybuffer[0x10000];
							chunk = datasize;
							if (chunk > sizeof(copybuffer))
								chunk = sizeof(copybuffer);

							VFS_READ(za->old->raw, copybuffer, chunk);
							datasize -= chunk;
						}
						//FIXME: graphics/progress updates.
					}
				}
			}

		//anything we didn't receive yet needs to be completed. the download code is meant to track the holes.
		*start = 0;
		*end = za->eocd.end;
		za->step++;
		break;
	default:
		return false;
	}
	return true;
}
struct archivedeltafuncs_s *FSZIP_OpenDeltaZip(qdownload_t *dl)
{
	ziparchivedelta_t *za;
	unsigned int trailsize = 0x10000 + SIZE_ENDOFCENTRALDIRECTORY+SIZE_ZIP64ENDOFCENTRALDIRECTORYLOCATOR;
	za->eocd.end = dl->size;
	if (dl->size < trailsize)
		za->eocd.start = 0;
	else
		za->eocd.start = dl->size - trailsize;

	za->dl = dl;
	za->zip.rawsize = dl->size;
	za->zip.raw = dl->file;
	za->pub.GetNextRange = FSZIPDL_GetNextRange;
	return &za->pub;
}
#endif

