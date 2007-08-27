/*
 *  Written by Jeroen Nijhof <jnijhof@nijhofnet.nl> 2005/03/01
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
 *  along with this program - see the file COPYING.
 */

/* --- includes --- */
#include <stdio.h>
#include <stdarg.h>			/* varg... */
#include <string.h>			/* strcmp,strncpy,... */
#include <sys/types.h>			/* stat */
#include <sys/stat.h>			/* stat */
#include <unistd.h>			/* stat,snprintf */
#if HAVE_VSYSLOG
#  include <syslog.h>			/* vsyslog */
#endif

/* enable these module-types */
#define PAM_SM_AUTH
#define PAM_SM_ACCOUNT
#define PAM_SM_SESSION
#define PAM_SM_PASSWORD

#include <security/pam_appl.h>		/* pam_* */
#include <security/pam_modules.h>
#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

/* --- customize these defines --- */

#ifndef PAM_SCRIPT_DIR
#  define PAM_SCRIPT_DIR	"/usr/bin/"
#endif
#define PAM_SCRIPT_AUTH		"pam_script_auth"
#define PAM_SCRIPT_ACCT		"pam_script_acct"
#define PAM_SCRIPT_PASSWD	"pam_script_passwd"
#define PAM_SCRIPT_SES_OPEN	"pam_script_ses_open"
#define PAM_SCRIPT_SES_CLOSE	"pam_script_ses_close"

/* --- defines --- */

#define PAM_EXTERN	extern
#define BUFSIZE	128
#define DEFAULT_USER "nobody"

/* --- macros --- */
#define PAM_SCRIPT_SETENV(key)						\
	{if (pam_get_item(pamh, key, &envval) == PAM_SUCCESS)		\
		pam_script_setenv(#key, (const char *)envval);}

#if 0
/* convenient function to throw into one of the methods below
 * for setting as a breakpoint for debugging purposes.
 */
void pam_script_xxx(void) {
	int i = 1;
}
#endif

/* internal helper functions */

static void pam_script_syslog(int priority, const char *format, ...) {
	va_list args;
	va_start(args, format);

#if HAVE_VSYSLOG
	openlog(PACKAGE, LOG_CONS|LOG_PID, LOG_AUTHPRIV);
	vsyslog(priority, format, args);
	closelog();
#else
	vfprintf(stderr, format, args);
#endif
}

static void pam_script_setenv(const char *key, const char *value) {
#if HAVE_SETENV
	setenv(key, value, 1);
#elif HAVE_PUTENV
	char	 buffer[BUFSIZE],
		*str;
	strncpy(buffer,key,BUFSIZE-2);
	strcat(buffer,"=");
	strncat(buffer,(value?value:""),BUFSIZE-strlen(buffer)-1);
	if ((str = (char *) malloc(strlen(buffer)+1)) != NULL) {
		strcpy(str,buffer);
		putenv(str);
	} /* else {
	     untrapped memory error - just do not add to environment
	} */
#else
#  error Can not set the environment
#endif
}

static int pam_script_get_user(pam_handle_t *pamh, const char **user) {
	int retval;

	retval = pam_get_user(pamh, user, NULL);
	if (retval != PAM_SUCCESS) {
		pam_script_syslog(LOG_ALERT, "pam_get_user returned error: %s",
			pam_strerror(pamh,retval));
		return retval;
	}
	if (*user == NULL || **user == '\0') {
		pam_script_syslog(LOG_ALERT, "username not known");
		retval = pam_set_item(pamh, PAM_USER,
			(const void *) DEFAULT_USER);
		if (retval != PAM_SUCCESS)
		return PAM_USER_UNKNOWN;
	}
	return retval;
}

static int pam_script_exec(pam_handle_t *pamh,
	const char *type, const char *script, const char *user,
	int rv, int argc, const char **argv) {

	int retval = rv;
	int i;
	char cmd[BUFSIZE];
	struct stat fs;
	const void *envval = NULL;

	/* check for pam.conf options */
	for (i = 1; i < argc; i++) {
		if (!strncmp(argv[i],"onerr=",6)) {
			if (!strcmp(argv[i],"onerr=fail"))
				retval = rv;
			else if (!strcmp(argv[i],"onerr=success"))
				retval = PAM_SUCCESS;
			else
				pam_script_syslog(LOG_ERR,
					"invalid option: %s", argv[i]);
		}
	}

	/* test for script existence first */
	snprintf(cmd, BUFSIZE, "%s%s", PAM_SCRIPT_DIR, script);
	if (stat(cmd, &fs) < 0) {
		/* stat failure */
		pam_script_syslog(LOG_ERR,"can not stat %s", cmd);
		return retval;
	}
	if ((fs.st_mode & (S_IXUSR|S_IXGRP|S_IXOTH))
	!= (S_IXUSR|S_IXGRP|S_IXOTH)) {
		/* script not executable at all levels */
		pam_script_syslog(LOG_ALERT,
			"script %s not fully executable", cmd);
		return retval;
	}

	/* Get PAM environment and place it in our environment */
	PAM_SCRIPT_SETENV(PAM_SERVICE);
	pam_script_setenv("PAM_TYPE", type);
	pam_script_setenv("PAM_USER", user);
	PAM_SCRIPT_SETENV(PAM_RUSER);
	PAM_SCRIPT_SETENV(PAM_RHOST);
	PAM_SCRIPT_SETENV(PAM_TTY);
	PAM_SCRIPT_SETENV(PAM_AUTHTOK);

	/* Execute external program */
	retval = system(cmd);
	if (retval)
		return rv;
	return PAM_SUCCESS;
}

/* --- authentication management functions --- */

PAM_EXTERN
int pam_sm_authenticate(pam_handle_t *pamh,int flags,int argc
			,const char **argv)
{
    int retval;
    const char *user=NULL;

    if ((retval = pam_script_get_user(pamh, &user)) != PAM_SUCCESS)
	return retval;

    return pam_script_exec(pamh, "auth", PAM_SCRIPT_AUTH,
	user, PAM_AUTH_ERR, argc, argv);
}

PAM_EXTERN
int pam_sm_setcred(pam_handle_t *pamh,int flags,int argc
		   ,const char **argv)
{
     return PAM_SUCCESS;
}

/* --- account management functions --- */

PAM_EXTERN
int pam_sm_acct_mgmt(pam_handle_t *pamh,int flags,int argc
		     ,const char **argv)
{
    int retval;
    const char *user=NULL;

    if ((retval = pam_script_get_user(pamh, &user)) != PAM_SUCCESS)
	return retval;

    return pam_script_exec(pamh, "account", PAM_SCRIPT_ACCT,
	user,PAM_AUTH_ERR,argc,argv);
}

/* --- password management --- */

PAM_EXTERN
int pam_sm_chauthtok(pam_handle_t *pamh,int flags,int argc
		     ,const char **argv)
{
     int retval;
     const char *user = NULL;
     char cmd[BUFSIZE];

     if ((retval = pam_script_get_user(pamh, &user)) != PAM_SUCCESS)
	return retval;

     if ( flags == PAM_UPDATE_AUTHTOK )
	return pam_script_exec(pamh, "password", PAM_SCRIPT_PASSWD,
		user, PAM_AUTHTOK_ERR, argc, argv);
     return PAM_SUCCESS;
}

/* --- session management --- */

PAM_EXTERN
int pam_sm_open_session(pam_handle_t *pamh,int flags,int argc
			,const char **argv)
{
     int retval;
     const char *user = NULL;
     char cmd[BUFSIZE];

     if ((retval = pam_script_get_user(pamh, &user)) != PAM_SUCCESS)
	return retval;

     return pam_script_exec(pamh, "session", PAM_SCRIPT_SES_OPEN,
	user, PAM_SESSION_ERR, argc, argv);
}

PAM_EXTERN
int pam_sm_close_session(pam_handle_t *pamh,int flags,int argc
			 ,const char **argv)
{
     int retval;
     const char *user = NULL;
     char cmd[BUFSIZE];

     if ((retval = pam_script_get_user(pamh, &user)) != PAM_SUCCESS)
	return retval;

     return pam_script_exec(pamh, "session", PAM_SCRIPT_SES_CLOSE,
	user, PAM_SESSION_ERR, argc, argv);
}

/* end of module definition */

#ifdef PAM_STATIC

/* static module data */

struct pam_module _pam_script_modstruct = {
    "pam_script",
    pam_sm_authenticate,
    pam_sm_setcred,
    pam_sm_acct_mgmt,
    pam_sm_open_session,
    pam_sm_close_session,
    pam_sm_chauthtok
};

#endif
