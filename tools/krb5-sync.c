/*
 * Command-line access to the krb5-sync kadmind plugin.
 *
 * This program provides command-line access to the functionality of the
 * krb5-sync kadmind plugin.  Using it, one can push password changes or
 * enabled/disabled status according to the same configuration used by the
 * plugin.  It's primarily intended for testing, but can also be used to
 * synchronize changes when the plugin previously failed for some reason.
 *
 * Written by Russ Allbery <rra@stanford.edu>
 * Based on code developed by Derrick Brashear and Ken Hornstein of Sine
 * Nomine Associates, on behalf of Stanford University.
 * Copyright 2006, 2007, 2010 Board of Trustees, Leland Stanford Jr. University
 *
 * See LICENSE for licensing terms.
 */

#include <config.h>
#include <portable/krb5.h>
#include <portable/system.h>

#include <errno.h>
#include <syslog.h>

#include <plugin/internal.h>
#include <util/messages-krb5.h>
#include <util/messages.h>


/*
 * Change a password in Active Directory.  Print a success message if we were
 * successful, and exit with an error message if we weren't.
 */
static void
ad_password(void *data, krb5_context ctx, krb5_principal principal,
            char *password, const char *user)
{
    char errbuf[BUFSIZ];
    int status;

    status = pwupdate_ad_change(data, ctx, principal, password,
                                strlen(password), errbuf, sizeof(errbuf));
    if (status != 0)
        die("AD password change for %s failed (%d): %s", user, status, errbuf);
    notice("AD password change for %s succeeded", user);
}


/*
 * Change the account status in Active Directory.  Print a success message if
 * we were successful, and exit with an error message if we weren't.
 */
static void
ad_status(void *data, krb5_context ctx, krb5_principal principal, int enable,
          const char *user)
{
    char errbuf[BUFSIZ];
    int status;

    status = pwupdate_ad_status(data, ctx, principal, enable, errbuf,
                                sizeof(errbuf));
    if (status != 0)
        die("AD status change for %s failed (%d): %s", user, status, errbuf);
    notice("AD status change for %s succeeded", user);
}


/*
 * Read a line from a queue file, making sure we got a complete line and
 * cutting off the trailing newline.  Doesn't return on error.
 */
static void
read_line(FILE *file, const char *filename, char *buffer, size_t bufsiz)
{
    if (fgets(buffer, bufsiz, file) == NULL)
        sysdie("cannot read from queue file %s", filename);
    if (buffer[strlen(buffer) - 1] != '\n')
        die("line too long in queue file %s", filename);
    buffer[strlen(buffer) - 1] = '\0';
}


/*
 * Read a queue file and take appropriate action based on its contents.  The
 * format is:
 *
 *     <principal>
 *     ad
 *     enable | disable | password
 *     [<password>]
 *
 * The actions are the same as from the command-line switches, except that
 * passwords are changed separately in AFS and AD.  enable and disable are not
 * supported for AFS.
 */
static void
process_queue_file(void *data, krb5_context ctx, const char *filename)
{
    FILE *queue;
    char buffer[BUFSIZ];
    char *user;
    krb5_principal principal;
    krb5_error_code ret;
    int ad = 0;
    int enable = 0;
    int disable = 0;
    int password = 0;

    queue = fopen(filename, "r");
    if (queue == NULL)
        sysdie("cannot open queue file %s", filename);

    /* Get user and convert into a principal. */
    read_line(queue, filename, buffer, sizeof(buffer));
    user = strdup(buffer);
    ret = krb5_parse_name(ctx, buffer, &principal);
    if (ret != 0)
        die_krb5(ctx, ret, "cannot parse user %s into principal", buffer);

    /* Get function. */
    read_line(queue, filename, buffer, sizeof(buffer));
    if (strcmp(buffer, "ad") == 0)
        ad = 1;
    else
        die("unknown target system %s in queue file %s", buffer, filename);
    read_line(queue, filename, buffer, sizeof(buffer));
    if (strcmp(buffer, "enable") == 0)
        enable = 1;
    else if (strcmp(buffer, "disable") == 0)
        disable = 1;
    else if (strcmp(buffer, "password") == 0)
        password = 1;
    else
        die("unknown action %s in queue file %s", buffer, filename);

    /* Perform the appropriate action. */
    if (password) {
        read_line(queue, filename, buffer, sizeof(buffer));
        if (ad)
            ad_password(data, ctx, principal, buffer, user);
    } else if (enable || disable) {
        ad_status(data, ctx, principal, enable, user);
    }

    /* If we got here, we were successful.  Close the file and delete it. */
    fclose(queue);
    if (unlink(filename) != 0)
        sysdie("unable to unlink queue file %s", filename);
    free(user);
}


int
main(int argc, char *argv[])
{
    int option;
    int enable = 0;
    int disable = 0;
    char *password = NULL;
    char *filename = NULL;
    char *user;
    void *data;
    krb5_context ctx;
    krb5_error_code ret;
    krb5_principal principal;

    /*
     * Actions should be logged to LOG_AUTH to go to the same place as the
     * logs from kadmind for easier log analysis.
     */
    openlog("krb5-sync", LOG_PID, LOG_AUTH);
    message_program_name = "krb5-sync";

    while ((option = getopt(argc, argv, "def:p:")) != EOF) {
        switch (option) {
        case 'd':
            disable = 1;
            break;
        case 'e':
            enable = 1;
            break;
        case 'f':
            filename = optarg;
            break;
        case 'p':
            password = optarg;
            break;
        default:
            fprintf(stderr, "Usage: krb5-sync [-d | -e] [-p <pass>] <user>\n");
            exit(1);
        }
    }
    argc -= optind;
    argv += optind;
    if (argc != 1 && filename == NULL) {
        fprintf(stderr, "Usage: krb5-sync [-d | -e] [-p <pass>] <user>\n");
        exit(1);
    }
    if (argc != 0 && filename != NULL) {
        fprintf(stderr, "Usage: krb5-sync -f <file>\n");
        exit(1);
    }
    user = argv[0];
    if (enable && disable)
        die("cannot specify both -d and -e");
    if (!enable && !disable && password == NULL && filename == NULL)
        die("no action specified");
    if (filename != NULL && (enable || disable || password != NULL))
        die("must specify queue file or action, not both");

    /* Create a Kerberos context for plugin initialization. */
    ret = krb5_init_context(&ctx);
    if (ret != 0)
        die_krb5(ctx, ret, "cannot initialize Kerberos context");

    /* Initialize the plugin. */
    if (pwupdate_init(ctx, &data))
        die("plugin initialization failed");

    /* Now, do whatever we were supposed to do. */
    if (filename != NULL)
        process_queue_file(data, ctx, filename);
    else {
        ret = krb5_parse_name(ctx, user, &principal);
        if (ret != 0)
            die_krb5(ctx, ret, "cannot parse user %s into principal", user);
        if (password != NULL)
            ad_password(data, ctx, principal, password, user);
        if (enable || disable)
            ad_status(data, ctx, principal, enable, user);
    }

    exit(0);
}
