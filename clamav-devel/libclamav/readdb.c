/*
 *  Copyright (C) 2002 - 2004 Tomasz Kojm <tkojm@clamav.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#if HAVE_CONFIG_H
#include "clamav-config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "clamav.h"
#include "cvd.h"
#include "strings.h"
#include "matcher.h"
#include "others.h"
#include "str.h"
#include "defaults.h"


static int cli_addsig(struct cl_node *root, const char *virname, const char *hexsig, int sigid, int parts, int partno, int type, unsigned int mindist, unsigned int maxdist)
{
	struct cli_patt *new;
	char *pt, *hex;
	int virlen, ret, i, error = 0;


    if((new = (struct cli_patt *) cli_calloc(1, sizeof(struct cli_patt))) == NULL)
	return CL_EMEM;

    new->type = type;
    new->sigid = sigid;
    new->parts = parts;
    new->partno = partno;
    new->mindist = mindist;
    new->maxdist = maxdist;

    if(strchr(hexsig, '(')) {
	    char *hexcpy, *hexnew, *start, *h;
	    short int *c;

	if(!(hexcpy = strdup(hexsig))) {
	    free(new);
	    return CL_EMEM;
	}

	if(!(hexnew = (char *) cli_calloc(strlen(hexsig) + 1, 1))) {
	    free(hexcpy);
	    free(new);
	    return CL_EMEM;
	}

	start = pt = hexcpy;
	while((pt = strchr(start, '('))) {
	    *pt++ = 0;

	    if(!start) {
		error = 1;
		break;
	    }

	    strcat(hexnew, start);
	    strcat(hexnew, "@@");

	    if(!(start = strchr(pt, ')'))) {
		error = 1;
		break;
	    }
	    *start++ = 0;

	    new->alt++;
	    new->altn = (unsigned short int *) realloc(new->altn, new->alt);
	    new->altn[new->alt - 1] = 0;
	    new->altc = (char **) realloc(new->altc, new->alt);

	    for(i = 0; i < strlen(pt); i++)
		if(pt[i] == '|')
		    new->altn[new->alt - 1]++;

	    if(!new->altn[new->alt - 1]) {
		error = 1;
		break;
	    } else
		new->altn[new->alt - 1]++;

	    if(!(new->altc[new->alt - 1] = (char *) cli_calloc(new->altn[new->alt - 1], 1))) {
		error = 1;
		break;
	    }

	    for(i = 0; i < new->altn[new->alt - 1]; i++) {
		if((h = cli_strtok(pt, i, "|")) == NULL) {
		    error = 1;
		    break;
		}

		if((c = cl_hex2str(h)) == NULL) {
		    free(h);
		    error = 1;
		    break;
		}

		new->altc[new->alt - 1][i] = (char) *c;
		free(c);
		free(h);
	    }

	    if(error)
		break;
	}

	if(start)
	    strcat(hexnew, start);

	hex = hexnew;
	free(hexcpy);

	if(error) {
	    free(hexcpy);
	    free(hexnew);
	    if(new->alt) {
		free(new->altn);
		for(i = 0; i < new->alt; i++)
		    free(new->altc[i]);
		free(new->altc);
	    }
	    free(new);
	    return CL_EMALFDB;
	}

    } else
	hex = (char *) hexsig;


    new->length = strlen(hex) / 2;

    if(new->length > root->maxpatlen)
	root->maxpatlen = new->length;

    if((new->pattern = cl_hex2str(hex)) == NULL) {
	if(new->alt) {
	    free(new->altn);
	    for(i = 0; i < new->alt; i++)
		free(new->altc[i]);
	    free(new->altc);
	    free(hex);
	}
	free(new);
	return CL_EMALFDB;
    }

    if((pt = strstr(virname, "(Clam)")))
	virlen = strlen(virname) - strlen(pt) - 1;
    else
	virlen = strlen(virname);

    if(virlen <= 0) {
	if(new->alt) {
	    free(new->altn);
	    for(i = 0; i < new->alt; i++)
		free(new->altc[i]);
	    free(new->altc);
	    free(hex);
	}
	free(new);
	return CL_EMALFDB;
    }

    if((new->virname = cli_calloc(virlen + 1, sizeof(char))) == NULL) {
	if(new->alt) {
	    free(new->altn);
	    for(i = 0; i < new->alt; i++)
		free(new->altc[i]);
	    free(new->altc);
	    free(hex);
	}
	free(new);
	return CL_EMEM;
    }

    strncpy(new->virname, virname, virlen);

    if((ret = cli_addpatt(root, new))) {
	free(new->virname);
	if(new->alt) {
	    free(new->altn);
	    for(i = 0; i < new->alt; i++)
		free(new->altc[i]);
	    free(new->altc);
	    free(hex);
	}
	free(new);
	return ret;
    }

    if(new->alt)
	free(hex);

    return 0;
}

int cli_parse_add(struct cl_node *root, char *virname, const char *hexsig, int type)
{
	struct cli_patt *new;
	char *pt, *hexcpy, *start, *n;
	int ret, virlen, parts = 0, i, len;
	int mindist = 0, maxdist = 0, error = 0;


    if(strchr(hexsig, '{')) {

	root->partsigs++;

	if(!(hexcpy = strdup(hexsig)))
	    return CL_EMEM;


	len = strlen(hexsig);
	for(i = 0; i < len; i++)
	    if(hexsig[i] == '{')
		parts++;

	if(parts)
	    parts++;

	start = pt = hexcpy;
	for(i = 1; i <= parts; i++) {

	    if(i != parts) {
		pt = strchr(start, '{');
		*pt++ = 0;
	    }

	    if((ret = cli_addsig(root, virname, start, root->partsigs, parts, i, type, mindist, maxdist))) {
		cli_errmsg("cli_parse_add(): Problem adding signature.\n");
		error = 1;
		break;
	    }

	    if(i == parts)
		break;

	    if(!(start = strchr(pt, '}'))) {
		error = 1;
		break;
	    }
	    *start++ = 0;

	    if(!pt) {
		error = 1;
		break;
	    }

	    mindist = maxdist = 0;
	    if(!strchr(pt, '-')) {
		if((mindist = maxdist = atoi(pt)) < 0) {
		    error = 1;
		    break;
		}
	    } else {
		if((n = cli_strtok(pt, 0, "-")) != NULL) {
		    if((mindist = atoi(n)) < 0) {
			error = 1;
			free(n);
			break;
		    }
		    free(n);
		}

		if((n = cli_strtok(pt, 1, "-")) != NULL) {
		    if((maxdist = atoi(n)) < 0) {
			error = 1;
			free(n);
			break;
		    }
		    free(n);
		}
	    }
	}

	free(hexcpy);
	if(error)
	    return CL_EMALFDB;

    } else if(strchr(hexsig, '*')) {
	root->partsigs++;

	len = strlen(hexsig);
	for(i = 0; i < len; i++)
	    if(hexsig[i] == '*')
		parts++;

	if(parts) /* there's always one part more */
	    parts++;

	for(i = 1; i <= parts; i++) {
	    if((pt = cli_strtok(hexsig, i - 1, "*")) == NULL) {
		cli_errmsg("Can't extract part %d of partial signature.\n", i + 1);
		return CL_EMALFDB;
	    }

	    if((ret = cli_addsig(root, virname, pt, root->partsigs, parts, i, type, 0, 0))) {
		cli_errmsg("cli_parse_add(): Problem adding signature.\n");
		free(pt);
		return ret;
	    }

	    free(pt);
	}

    } else { /* static */
	if((ret = cli_addsig(root, virname, hexsig, 0, 0, 0, type, 0, 0))) {
	    cli_errmsg("cli_parse_add(): Problem adding signature.\n");
	    return ret;
	}
    }

    return 0;
}

int cl_loaddb(const char *filename, struct cl_node **root, int *virnum)
{
	FILE *fd;
	char *buffer, *pt, *start;
	int line = 0, ret;


    if((fd = fopen(filename, "rb")) == NULL) {
	cli_errmsg("cl_loaddb(): Can't open file %s\n", filename);
	return CL_EOPEN;
    }

    cli_dbgmsg("Loading %s\n", filename);

    if(!(buffer = (char *) cli_malloc(FILEBUFF))) {
	fclose(fd);
	return CL_EMEM;
    }

    memset(buffer, 0, FILEBUFF);

    /* test for CVD file */

    if (fgets(buffer, 12, fd) == NULL) {
	cli_dbgmsg("%s: failure reading header\n", filename);
	free(buffer);
	fclose(fd);
	return CL_EMALFDB;
    }

    rewind(fd);

    if(!strncmp(buffer, "ClamAV-VDB:", 11)) {
	cli_dbgmsg("%s: CVD file detected\n", filename);
	ret = cli_cvdload(fd, root, virnum);
	free(buffer);
	fclose(fd);
	return ret;
    }

    while(fgets(buffer, FILEBUFF, fd)) {

	line++;
	cli_chomp(buffer);

	pt = strchr(buffer, '=');
	if(!pt) {
	    cli_errmsg("readdb(): Malformed pattern line %d (file %s).\n", line, filename);
	    free(buffer);
	    fclose(fd);
	    return CL_EMALFDB;
	}

	start = buffer;
	*pt++ = 0;

	if(*pt == '=') continue;

	if(!*root) {
	    cli_dbgmsg("Initializing trie.\n");
	    *root = (struct cl_node *) cli_calloc(1, sizeof(struct cl_node));
	    if(!*root) {
		free(buffer);
		fclose(fd);
		return CL_EMEM;
	    }
	    (*root)->maxpatlen = 0;
	}

	if((ret = cli_parse_add(*root, start, pt, 0))) {
	    cli_errmsg("readdb(): Problem parsing signature at line %d (file %s).\n", line, filename);
	    free(buffer);
	    fclose(fd);
	    return ret;
	}
    }

    if(virnum != NULL)
	*virnum += line;

    free(buffer);
    fclose(fd);

    return 0;
}

const char *cl_retdbdir(void)
{
    return DATADIR;
}

int cl_loaddbdir(const char *dirname, struct cl_node **root, int *virnum)
{
	DIR *dd;
	struct dirent *dent;
	char *dbfile;
	int ret;


    if((dd = opendir(dirname)) == NULL) {
        cli_errmsg("cl_loaddbdir(): Can't open directory %s\n", dirname);
        return CL_EOPEN;
    }

    cli_dbgmsg("Loading databases from %s\n", dirname);

    while((dent = readdir(dd))) {
#ifndef C_INTERIX
	if(dent->d_ino)
#endif
	{
	    if(strcmp(dent->d_name, ".") && strcmp(dent->d_name, "..") &&
	    (cli_strbcasestr(dent->d_name, ".db")  ||
	     cli_strbcasestr(dent->d_name, ".db2") ||
	     cli_strbcasestr(dent->d_name, ".cvd"))) {

		dbfile = (char *) cli_calloc(strlen(dent->d_name) + strlen(dirname) + 2, sizeof(char));

		if(!dbfile) {
		    cli_dbgmsg("cl_loaddbdir(): dbfile == NULL\n");
		    closedir(dd);
		    return CL_EMEM;
		}
		sprintf(dbfile, "%s/%s", dirname, dent->d_name);
		if((ret = cl_loaddb(dbfile, root, virnum))) {
		    cli_dbgmsg("cl_loaddbdir(): error loading database %s\n", dbfile);
		    free(dbfile);
		    closedir(dd);
		    return ret;
		}
		free(dbfile);
	    }
	}
    }

    closedir(dd);
    return 0;
}

int cl_statinidir(const char *dirname, struct cl_stat *dbstat)
{
	DIR *dd;
	struct dirent *dent;
        char *fname;


    if(dbstat) {
	dbstat->no = 0;
	dbstat->stattab = NULL;
	dbstat->dir = strdup(dirname);
    } else {
        cli_errmsg("cl_statdbdir(): Null argument passed.\n");
	return CL_ENULLARG;
    }

    if((dd = opendir(dirname)) == NULL) {
        cli_errmsg("cl_statdbdir(): Can't open directory %s\n", dirname);
        return CL_EOPEN;
    }

    cli_dbgmsg("Stat()ing files in %s\n", dirname);

    while((dent = readdir(dd))) {
#ifndef C_INTERIX
	if(dent->d_ino)
#endif
	{
	    if(strcmp(dent->d_name, ".") && strcmp(dent->d_name, "..") && (cli_strbcasestr(dent->d_name, ".db") || cli_strbcasestr(dent->d_name, ".db2") || cli_strbcasestr(dent->d_name, ".cvd"))) {

		dbstat->no++;
		dbstat->stattab = (struct stat *) realloc(dbstat->stattab, dbstat->no * sizeof(struct stat));
                fname = cli_calloc(strlen(dirname) + strlen(dent->d_name) + 2, sizeof(char));
		sprintf(fname, "%s/%s", dirname, dent->d_name);
		stat(fname, &dbstat->stattab[dbstat->no - 1]);
		free(fname);
	    }
	}
    }

    closedir(dd);
    return 0;
}

int cl_statchkdir(const struct cl_stat *dbstat)
{
	DIR *dd;
	struct dirent *dent;
	struct stat sb;
	int i, found;
	char *fname;


    if(!dbstat || !dbstat->dir) {
        cli_errmsg("cl_statdbdir(): Null argument passed.\n");
	return CL_ENULLARG;
    }

    if((dd = opendir(dbstat->dir)) == NULL) {
        cli_errmsg("cl_statdbdir(): Can't open directory %s\n", dbstat->dir);
        return CL_EOPEN;
    }

    cli_dbgmsg("Stat()ing files in %s\n", dbstat->dir);

    while((dent = readdir(dd))) {
#ifndef C_INTERIX
	if(dent->d_ino)
#endif
	{
	    if(strcmp(dent->d_name, ".") && strcmp(dent->d_name, "..") && (cli_strbcasestr(dent->d_name, ".db") || cli_strbcasestr(dent->d_name, ".db2") || cli_strbcasestr(dent->d_name, ".cvd"))) {

                fname = cli_calloc(strlen(dbstat->dir) + strlen(dent->d_name) + 2, sizeof(char));
		sprintf(fname, "%s/%s", dbstat->dir, dent->d_name);
		stat(fname, &sb);
		free(fname);

		found = 0;
		for(i = 0; i < dbstat->no; i++)
		    if(dbstat->stattab[i].st_ino == sb.st_ino) {
			found = 1;
			if(dbstat->stattab[i].st_mtime != sb.st_mtime) {
			    closedir(dd);
			    return 1;
			}
		    }

		if(!found) {
		    closedir(dd);
		    return 1;
		}
	    }
	}
    }

    closedir(dd);
    return 0;
}

int cl_statfree(struct cl_stat *dbstat)
{

    if(dbstat) {
	free(dbstat->stattab);
	dbstat->stattab = NULL;
	dbstat->no = 0;
	if(dbstat->dir) {
	    free(dbstat->dir);
	    dbstat->dir = NULL;
	}
    } else {
        cli_errmsg("cl_statfree(): Null argument passed.\n");
	return CL_ENULLARG;
    }

    return 0;
}
