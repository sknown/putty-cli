/*
 * putty-cli.c - Command-line (CLI) version of PuTTY.
 *
 * This provides a CLI version of the putty program that can run
 * without GTK, for use on systems (like macOS) where GTK is not
 * installed. It works similarly to plink but carries the putty
 * branding and putty-style command-line options.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <stdarg.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "putty.h"
#include "ssh.h"
#include "storage.h"
#include "tree234.h"

#define MAX_STDIN_BACKLOG 4096

static LogContext *logctx;

static struct termios orig_termios;

void cmdline_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    console_print_error_msg_fmt_v("putty", fmt, ap);
    va_end(ap);
    exit(1);
}

static bool local_tty = false; /* do we have a local tty? */

static Backend *backend;
static Conf *conf;

/* Daemon / client mode */
static bool daemon_mode = false;
static char *daemon_socket_path = NULL;
static int daemon_listen_fd = -1;
static int daemon_client_fd = -1;
static bool daemon_client_input_done = false;
static bufchain daemon_client_input_buf;
static bool client_mode = false;
static char *client_socket_path = NULL;

/*
 * Default settings that are specific to Unix putty.
 */
char *platform_default_s(const char *name)
{
    if (!strcmp(name, "TermType"))
        return dupstr(getenv("TERM"));
    if (!strcmp(name, "SerialLine"))
        return dupstr("/dev/ttyS0");
    return NULL;
}

bool platform_default_b(const char *name, bool def)
{
    return def;
}

int platform_default_i(const char *name, int def)
{
    return def;
}

FontSpec *platform_default_fontspec(const char *name)
{
    return fontspec_new_default();
}

Filename *platform_default_filename(const char *name)
{
    if (!strcmp(name, "LogFileName"))
        return filename_from_str("putty.log");
    else
        return filename_from_str("");
}

char *x_get_default(const char *key)
{
    return NULL;                       /* this is a stub */
}

/*
 * Daemon mode: Unix domain socket management
 */
static int create_daemon_socket(const char *path)
{
    struct sockaddr_un addr;
    int fd, ret;

    unlink(path);    /* remove existing socket file */

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "putty: unable to create daemon socket: %s\n",
                strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    ret = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0) {
        fprintf(stderr, "putty: unable to bind daemon socket '%s': %s\n",
                path, strerror(errno));
        close(fd);
        return -1;
    }

    ret = listen(fd, 5);
    if (ret < 0) {
        fprintf(stderr, "putty: unable to listen on daemon socket: %s\n",
                strerror(errno));
        close(fd);
        unlink(path);
        return -1;
    }

    nonblock(fd);
    cloexec(fd);
    return fd;
}

static int accept_daemon_client(int listen_fd)
{
    struct sockaddr_un addr;
    socklen_t addrlen = sizeof(addr);
    int fd = accept(listen_fd, (struct sockaddr *)&addr, &addrlen);
    if (fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            fprintf(stderr, "putty: accept: %s\n", strerror(errno));
        return -1;
    }
    nonblock(fd);
    return fd;
}

static void close_daemon_client(void)
{
    if (daemon_client_fd >= 0) {
        close(daemon_client_fd);
        daemon_client_fd = -1;
    }
    bufchain_clear(&daemon_client_input_buf);
    daemon_client_input_done = false;
}

static void cleanup_daemon_socket(void)
{
    if (daemon_listen_fd >= 0) {
        close(daemon_listen_fd);
        daemon_listen_fd = -1;
    }
    if (daemon_socket_path) {
        unlink(daemon_socket_path);
        sfree(daemon_socket_path);
        daemon_socket_path = NULL;
    }
}

static void putty_cli_echoedit_update(Seat *seat, bool echo, bool edit)
{
    /* Update stdin read mode to reflect changes in line discipline. */
    struct termios mode;

    if (!local_tty) return;

    mode = orig_termios;

    if (echo)
        mode.c_lflag |= ECHO;
    else
        mode.c_lflag &= ~ECHO;

    if (edit) {
        mode.c_iflag |= ICRNL;
        mode.c_lflag |= ISIG | ICANON;
        mode.c_oflag |= OPOST;
    } else {
        mode.c_iflag &= ~ICRNL;
        mode.c_lflag &= ~(ISIG | ICANON);
        mode.c_oflag &= ~OPOST;
        mode.c_cc[VMIN] = 1;
        mode.c_cc[VTIME] = 0;
        mode.c_iflag &= ~IXON;
        mode.c_iflag &= ~IXOFF;
    }
    mode.c_iflag = (mode.c_iflag | INPCK | PARMRK) & ~IGNPAR;

    tcsetattr(STDIN_FILENO, TCSANOW, &mode);
}

/* Helper function to extract a special character from a termios. */
static char *get_ttychar(struct termios *t, int index)
{
    cc_t c = t->c_cc[index];
#if defined(_POSIX_VDISABLE)
    if (c == _POSIX_VDISABLE)
        return dupstr("");
#endif
    return dupprintf("^<%d>", c);
}

static char *putty_cli_get_ttymode(Seat *seat, const char *mode)
{
    if (!local_tty) return NULL;

#define GET_CHAR(ourname, uxname) \
    do { \
        if (strcmp(mode, ourname) == 0) \
            return get_ttychar(&orig_termios, uxname); \
    } while (0)
#define GET_BOOL(ourname, uxname, uxmemb, transform) \
    do { \
        if (strcmp(mode, ourname) == 0) { \
            bool b = (orig_termios.uxmemb & uxname) != 0; \
            transform; \
            return dupprintf("%d", b); \
        } \
    } while (0)

    /* All the special characters supported by SSH */
#if defined(VINTR)
    GET_CHAR("INTR", VINTR);
#endif
#if defined(VQUIT)
    GET_CHAR("QUIT", VQUIT);
#endif
#if defined(VERASE)
    GET_CHAR("ERASE", VERASE);
#endif
#if defined(VKILL)
    GET_CHAR("KILL", VKILL);
#endif
#if defined(VEOF)
    GET_CHAR("EOF", VEOF);
#endif
#if defined(VEOL)
    GET_CHAR("EOL", VEOL);
#endif
#if defined(VEOL2)
    GET_CHAR("EOL2", VEOL2);
#endif
#if defined(VSTART)
    GET_CHAR("START", VSTART);
#endif
#if defined(VSTOP)
    GET_CHAR("STOP", VSTOP);
#endif
#if defined(VSUSP)
    GET_CHAR("SUSP", VSUSP);
#endif
#if defined(VDSUSP)
    GET_CHAR("DSUSP", VDSUSP);
#endif
#if defined(VREPRINT)
    GET_CHAR("REPRINT", VREPRINT);
#endif
#if defined(VWERASE)
    GET_CHAR("WERASE", VWERASE);
#endif
#if defined(VLNEXT)
    GET_CHAR("LNEXT", VLNEXT);
#endif
#if defined(VFLUSH)
    GET_CHAR("FLUSH", VFLUSH);
#endif
#if defined(VSWTCH)
    GET_CHAR("SWTCH", VSWTCH);
#endif
#if defined(VSTATUS)
    GET_CHAR("STATUS", VSTATUS);
#endif
#if defined(VDISCARD)
    GET_CHAR("DISCARD", VDISCARD);
#endif
    /* Modes that "configure" other major modes */
#if defined(ECHOK)
    GET_BOOL("ECHOK", ECHOK, c_lflag, );
#endif
#if defined(ECHOKE)
    GET_BOOL("ECHOKE", ECHOKE, c_lflag, );
#endif
#if defined(ECHOE)
    GET_BOOL("ECHOE", ECHOE, c_lflag, );
#endif
#if defined(ECHONL)
    GET_BOOL("ECHONL", ECHONL, c_lflag, );
#endif
#if defined(XCASE)
    GET_BOOL("XCASE", XCASE, c_lflag, );
#endif
#if defined(IUTF8)
    GET_BOOL("IUTF8", IUTF8, c_iflag, );
#endif
#if defined(ECHOCTL)
    GET_BOOL("ECHOCTL", ECHOCTL, c_lflag, );
#endif
#if defined(IXANY)
    GET_BOOL("IXANY", IXANY, c_iflag, );
#endif
#if defined(OLCUC)
    GET_BOOL("OLCUC", OLCUC, c_oflag, );
#endif
#if defined(ONLCR)
    GET_BOOL("ONLCR", ONLCR, c_oflag, );
#endif
#if defined(OCRNL)
    GET_BOOL("OCRNL", OCRNL, c_oflag, );
#endif
#if defined(ONOCR)
    GET_BOOL("ONOCR", ONOCR, c_oflag, );
#endif
#if defined(ONLRET)
    GET_BOOL("ONLRET", ONLRET, c_oflag, );
#endif
#if defined(ISIG)
    GET_BOOL("ISIG", ISIG, c_lflag, );
#endif
#if defined(ICANON)
    GET_BOOL("ICANON", ICANON, c_lflag, );
#endif
#if defined(ECHO)
    GET_BOOL("ECHO", ECHO, c_lflag, );
#endif
#if defined(IXON)
    GET_BOOL("IXON", IXON, c_iflag, );
#endif
#if defined(IXOFF)
    GET_BOOL("IXOFF", IXOFF, c_iflag, );
#endif
#if defined(OPOST)
    GET_BOOL("OPOST", OPOST, c_oflag, );
#endif

#undef GET_CHAR
#undef GET_BOOL

    return NULL;
}

void cleanup_termios(void)
{
    if (local_tty)
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

static bufchain stdout_data, stderr_data;
static bufchain_sink stdout_bcs, stderr_bcs;
static StripCtrlChars *stdout_scc, *stderr_scc;
static BinarySink *stdout_bs, *stderr_bs;

static enum { EOF_NO, EOF_PENDING, EOF_SENT } outgoingeof;

static size_t output_backlog(void)
{
    return bufchain_size(&stdout_data) + bufchain_size(&stderr_data);
}

void try_output(bool is_stderr)
{
    bufchain *chain = (is_stderr ? &stderr_data : &stdout_data);
    int fd;

    if (daemon_mode && daemon_client_fd >= 0) {
        fd = daemon_client_fd;
    } else if (daemon_mode) {
        /* Daemon mode with no client: discard output silently */
        bufchain_clear(chain);
        backend_unthrottle(backend, output_backlog());
        return;
    } else {
        fd = (is_stderr ? STDERR_FILENO : STDOUT_FILENO);
    }
    ssize_t ret;

    if (bufchain_size(chain) > 0) {
        bool prev_nonblock = nonblock(fd);
        ptrlen senddata;
        do {
            senddata = bufchain_prefix(chain);
            ret = write(fd, senddata.ptr, senddata.len);
            if (ret > 0)
                bufchain_consume(chain, ret);
        } while (ret == senddata.len && bufchain_size(chain) != 0);
        if (!prev_nonblock)
            no_nonblock(fd);
        if (ret < 0 && errno != EAGAIN) {
            perror(is_stderr ? "stderr: write" : "stdout: write");
            exit(1);
        }

        backend_unthrottle(backend, output_backlog());
    }
    if (outgoingeof == EOF_PENDING && bufchain_size(&stdout_data) == 0) {
        close(STDOUT_FILENO);
        outgoingeof = EOF_SENT;
    }
}

static size_t putty_cli_output(
    Seat *seat, SeatOutputType type, const void *data, size_t len)
{
    bool is_stderr = type != SEAT_OUTPUT_STDOUT;
    assert(is_stderr || outgoingeof == EOF_NO);

    BinarySink *bs = is_stderr ? stderr_bs : stdout_bs;
    put_data(bs, data, len);

    try_output(is_stderr);
    return output_backlog();
}

static bool putty_cli_eof(Seat *seat)
{
    assert(outgoingeof == EOF_NO);
    outgoingeof = EOF_PENDING;
    try_output(false);
    return false;   /* do not respond to incoming EOF with outgoing */
}

static SeatPromptResult putty_cli_get_userpass_input(Seat *seat, prompts_t *p)
{
    static cmdline_get_passwd_input_state cmdline_state =
        CMDLINE_GET_PASSWD_INPUT_STATE_INIT;

    SeatPromptResult spr;
    spr = cmdline_get_passwd_input(p, &cmdline_state, false);
    if (spr.kind == SPRK_INCOMPLETE)
        spr = console_get_userpass_input(p);
    return spr;
}

static bool putty_cli_seat_interactive(Seat *seat)
{
    return (!*conf_get_str_ambi(conf, CONF_remote_cmd, NULL) &&
            !*conf_get_str_ambi(conf, CONF_remote_cmd2, NULL) &&
            !*conf_get_str(conf, CONF_ssh_nc_host));
}

/*
 * In daemon mode, auto-accept unknown host keys without caching
 * (equivalent to pressing 'n' at the interactive prompt).
 */
static SeatPromptResult putty_cli_confirm_ssh_host_key(
    Seat *seat, const char *host, int port, const char *keytype,
    char *keystr, SeatDialogText *text, HelpCtx helpctx,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    if (daemon_mode) {
        /* Accept without storing - daemon is non-interactive */
        return SPR_OK;
    }
    return console_confirm_ssh_host_key(
        seat, host, port, keytype, keystr, text, helpctx,
        callback, ctx);
}

static const SeatVtable putty_cli_seat_vt = {
    .output = putty_cli_output,
    .eof = putty_cli_eof,
    .sent = nullseat_sent,
    .banner = nullseat_banner_to_stderr,
    .get_userpass_input = putty_cli_get_userpass_input,
    .notify_session_started = nullseat_notify_session_started,
    .notify_remote_exit = nullseat_notify_remote_exit,
    .notify_remote_disconnect = nullseat_notify_remote_disconnect,
    .connection_fatal = console_connection_fatal,
    .nonfatal = console_nonfatal,
    .update_specials_menu = nullseat_update_specials_menu,
    .get_ttymode = putty_cli_get_ttymode,
    .set_busy_status = nullseat_set_busy_status,
    .confirm_ssh_host_key = putty_cli_confirm_ssh_host_key,
    .confirm_weak_crypto_primitive = console_confirm_weak_crypto_primitive,
    .confirm_weak_cached_hostkey = console_confirm_weak_cached_hostkey,
    .prompt_descriptions = console_prompt_descriptions,
    .is_utf8 = nullseat_is_never_utf8,
    .echoedit_update = putty_cli_echoedit_update,
    .get_x_display = nullseat_get_x_display,
    .get_windowid = nullseat_get_windowid,
    .get_window_pixel_size = nullseat_get_window_pixel_size,
    .stripctrl_new = console_stripctrl_new,
    .set_trust_status = console_set_trust_status,
    .can_set_trust_status = console_can_set_trust_status,
    .has_mixed_input_stream = console_has_mixed_input_stream,
    .verbose = cmdline_seat_verbose,
    .interactive = putty_cli_seat_interactive,
    .get_cursor_position = nullseat_get_cursor_position,
};
static Seat putty_cli_seat[1] = {{ &putty_cli_seat_vt }};

/*
 * Handle data from a local tty in PARMRK format.
 */
static void from_tty(void *vbuf, unsigned len)
{
    char *p, *q, *end, *buf = vbuf;
    static enum {NORMAL, FF, FF00} state = NORMAL;

    p = buf; end = buf + len;
    while (p < end) {
        switch (state) {
          case NORMAL:
            if (*p == '\xff') {
                p++;
                state = FF;
            } else {
                q = memchr(p, '\xff', end - p);
                if (q == NULL) q = end;
                backend_send(backend, p, q - p);
                p = q;
            }
            break;
          case FF:
            if (*p == '\xff') {
                backend_send(backend, p, 1);
                p++;
                state = NORMAL;
            } else if (*p == '\0') {
                p++;
                state = FF00;
            } else abort();
            break;
          case FF00:
            if (*p == '\0') {
                backend_special(backend, SS_BRK, 0);
            } else {
                if (orig_termios.c_iflag & INPCK) {
                    if (!(orig_termios.c_iflag & IGNPAR)) {
                        *p = 0;
                        backend_send(backend, p, 1);
                    }
                } else {
                    backend_send(backend, p, 1);
                }
            }
            p++;
            state = NORMAL;
        }
    }
}

static int signalpipe[2];
static volatile sig_atomic_t sigint_pending = false;

void sigwinch(int signum)
{
    if (write(signalpipe[1], "x", 1) <= 0)
        /* not much we can do about it */;
}

void sigint(int signum)
{
    /* In daemon mode, Ctrl+C should terminate the process.
     * Set a flag and wake up the main loop by writing to the
     * signal pipe, so poll() returns instead of blocking forever. */
    sigint_pending = true;
    if (write(signalpipe[1], "i", 1) <= 0)
        /* not much we can do about it */;
}

/*
 * Short description of parameters.
 */
static void usage(void)
{
    printf("putty: command-line connection utility\n");
    printf("%s\n", ver);
    printf("Usage: putty [options] [user@]host [command]\n");
    printf("       (\"host\" can also be a PuTTY saved session name)\n");
    printf("Options:\n");
    printf("  -V        print version information and exit\n");
    printf("  -pgpfp    print PGP key fingerprints and exit\n");
    printf("  -v        show verbose messages\n");
    printf("  -load sessname  Load settings from saved session\n");
    printf("  -ssh -telnet -rlogin -raw -serial\n");
    printf("            force use of a particular protocol\n");
    printf("  -ssh-connection\n");
    printf("            force use of the bare ssh-connection protocol\n");
    printf("  -P port   connect to specified port\n");
    printf("  -l user   connect with specified username\n");
    printf("  -batch    disable all interactive prompts\n");
    printf("  -proxycmd command\n");
    printf("            use 'command' as local proxy\n");
    printf("  -sercfg configuration-string (e.g. 19200,8,n,1,X)\n");
    printf("            Specify the serial configuration (serial only)\n");
    printf("The following options only apply to SSH connections:\n");
    printf("  -pwfile file   login with password read from specified file\n");
    printf("  -D [listen-IP:]listen-port\n");
    printf("            Dynamic SOCKS-based port forwarding\n");
    printf("  -L [listen-IP:]listen-port:host:port\n");
    printf("            Forward local port to remote address\n");
    printf("  -R [listen-IP:]listen-port:host:port\n");
    printf("            Forward remote port to local address\n");
    printf("  -X -x     enable / disable X11 forwarding\n");
    printf("  -A -a     enable / disable agent forwarding\n");
    printf("  -t -T     enable / disable pty allocation\n");
    printf("  -1 -2     force use of particular SSH protocol version\n");
    printf("  -4 -6     force use of IPv4 or IPv6\n");
    printf("  -C        enable compression\n");
    printf("  -i key    private key file for user authentication\n");
    printf("  -noagent  disable use of Pageant\n");
    printf("  -agent    enable use of Pageant\n");
    printf("  -no-trivial-auth\n");
    printf("            disconnect if SSH authentication succeeds trivially\n");
    printf("  -noshare  disable use of connection sharing\n");
    printf("  -share    enable use of connection sharing\n");
    printf("  -hostkey keyid\n");
    printf("            manually specify a host key (may be repeated)\n");
    printf("  -sanitise-stderr, -sanitise-stdout, "
           "-no-sanitise-stderr, -no-sanitise-stdout\n");
    printf("            do/don't strip control chars from standard "
           "output/error\n");
    printf("  -no-antispoof   omit anti-spoofing prompt after "
           "authentication\n");
    printf("  -m file   read remote command(s) from file\n");
    printf("  -s        remote command is an SSH subsystem (SSH-2 only)\n");
    printf("  -N        don't start a shell/command (SSH-2 only)\n");
    printf("  -nc host:port\n");
    printf("            open tunnel in place of session (SSH-2 only)\n");
    printf("  -sshlog file\n");
    printf("  -sshrawlog file\n");
    printf("            log protocol details to a file\n");
    printf("  -logoverwrite\n");
    printf("  -logappend\n");
    printf("            control what happens when a log file already exists\n");
    printf("  -shareexists\n");
    printf("            test whether a connection-sharing upstream exists\n");
    printf("  --daemon socket-path\n");
    printf("            run in daemon mode, listening on Unix socket\n");
    printf("            (persistent SSH connection for AI tooling)\n");
    printf("  --connect socket-path [command]\n");
    printf("            connect to a running putty-cli daemon\n");
    printf("            optional command is sent directly (no pipe needed)\n");
}

static void version(void)
{
    char *buildinfo_text = buildinfo("\n");
    printf("putty: %s\n%s\n", ver, buildinfo_text);
    sfree(buildinfo_text);
    exit(0);
}

void frontend_net_error_pending(void) {}

const bool share_can_be_downstream = true;
const bool share_can_be_upstream = true;

const bool buildinfo_gtk_relevant = false;

const unsigned cmdline_tooltype =
    TOOLTYPE_HOST_ARG |
    TOOLTYPE_PORT_ARG |
    TOOLTYPE_HOST_ARG_CAN_BE_SESSION |
    TOOLTYPE_HOST_ARG_PROTOCOL_PREFIX |
    TOOLTYPE_HOST_ARG_FROM_LAUNCHABLE_LOAD;

static bool seen_stdin_eof = false;

static bool putty_cli_pw_setup(void *vctx, pollwrapper *pw)
{
    pollwrap_add_fd_rwx(pw, signalpipe[0], SELECT_R);

    if (!seen_stdin_eof &&
        backend_connected(backend) &&
        backend_sendok(backend) &&
        backend_sendbuffer(backend) < MAX_STDIN_BACKLOG) {
        pollwrap_add_fd_rwx(pw, STDIN_FILENO, SELECT_R);
    }

    if (bufchain_size(&stdout_data) > 0) {
        pollwrap_add_fd_rwx(pw, STDOUT_FILENO, SELECT_W);
    }

    if (bufchain_size(&stderr_data) > 0) {
        pollwrap_add_fd_rwx(pw, STDERR_FILENO, SELECT_W);
    }

    return true;
}

static void putty_cli_pw_check(void *vctx, pollwrapper *pw)
{
    if (pollwrap_check_fd_rwx(pw, signalpipe[0], SELECT_R)) {
        char c[1];
        struct winsize size;
        if (read(signalpipe[0], c, 1) <= 0)
            /* ignore error */;
        if (ioctl(STDIN_FILENO, TIOCGWINSZ, (void *)&size) >= 0)
            backend_size(backend, size.ws_col, size.ws_row);
    }

    if (pollwrap_check_fd_rwx(pw, STDIN_FILENO, SELECT_R)) {
        char buf[4096];
        int ret;

        if (backend_connected(backend)) {
            ret = read(STDIN_FILENO, buf, sizeof(buf));
            noise_ultralight(NOISE_SOURCE_IOLEN, ret);
            if (ret < 0) {
                perror("stdin: read");
                exit(1);
            } else if (ret == 0) {
                backend_special(backend, SS_EOF, 0);
                seen_stdin_eof = true;
            } else {
                if (local_tty)
                    from_tty(buf, ret);
                else
                    backend_send(backend, buf, ret);
            }
        }
    }

    if (pollwrap_check_fd_rwx(pw, STDOUT_FILENO, SELECT_W))
        try_output(false);

    if (pollwrap_check_fd_rwx(pw, STDERR_FILENO, SELECT_W))
        try_output(true);
}

static bool putty_cli_continue(void *vctx, bool found_any_fd,
                               bool ran_any_callback)
{
    if (!backend_connected(backend) &&
        bufchain_size(&stdout_data) == 0 && bufchain_size(&stderr_data) == 0)
        return false;                  /* terminate main loop */
    return true;
}

/*
 * Daemon mode event loop functions.
 *
 * In daemon mode, instead of reading from stdin and writing to stdout,
 * we listen on a Unix domain socket and relay I/O with connected clients.
 * The SSH connection stays alive between client invocations.
 */
static bool putty_daemon_pw_setup(void *vctx, pollwrapper *pw)
{
    /* Check SIGINT flag directly — don't rely solely on signalpipe,
     * since the main loop may restart poll() on EINTR and miss it. */
    if (sigint_pending)
        return false;

    pollwrap_add_fd_rwx(pw, signalpipe[0], SELECT_R);

    /* Daemon mode: ensure ISIG stays on and monitor stdin for Ctrl+C.
     *
     * Some code paths (e.g. console_get_userpass_input) may temporarily
     * modify terminal settings. In rare cases ISIG can end up off, which
     * prevents Ctrl+C from generating SIGINT. We restore ISIG here as
     * a watchdog, and also poll stdin so that if ISIG IS off, we can
     * detect Ctrl+C (0x03) as regular input. */
    if (daemon_mode && isatty(STDIN_FILENO)) {
        struct termios tio;
        if (tcgetattr(STDIN_FILENO, &tio) == 0) {
            if (!(tio.c_lflag & ISIG)) {
                tio.c_lflag |= ISIG;
                tcsetattr(STDIN_FILENO, TCSANOW, &tio);
            }
        }
        pollwrap_add_fd_rwx(pw, STDIN_FILENO, SELECT_R);
    }

    /* Always listen for new client connections */
    if (daemon_listen_fd >= 0) {
        pollwrap_add_fd_rwx(pw, daemon_listen_fd, SELECT_R);
    }

    /* Flush any buffered client input now that backend may be ready */
    if (bufchain_size(&daemon_client_input_buf) > 0) {
        if (backend_connected(backend) && backend_sendok(backend)) {
            while (bufchain_size(&daemon_client_input_buf) > 0 &&
                   backend_sendok(backend)) {
                ptrlen data = bufchain_prefix(&daemon_client_input_buf);
                backend_send(backend, data.ptr, data.len);
                bufchain_consume(&daemon_client_input_buf, data.len);
            }
        }
    }

    /* If we have a connected client, handle I/O */
    if (daemon_client_fd >= 0) {
        /* Poll for reading from client if buffer isn't too large */
        if (!daemon_client_input_done &&
            bufchain_size(&daemon_client_input_buf) < MAX_STDIN_BACKLOG) {
            pollwrap_add_fd_rwx(pw, daemon_client_fd, SELECT_R);
        }

        if (bufchain_size(&stdout_data) > 0 ||
            bufchain_size(&stderr_data) > 0) {
            pollwrap_add_fd_rwx(pw, daemon_client_fd, SELECT_W);
        }
    }

    return true;
}

static void putty_daemon_pw_check(void *vctx, pollwrapper *pw)
{
    /* Handle signal pipe (SIGWINCH / SIGINT) */
    if (pollwrap_check_fd_rwx(pw, signalpipe[0], SELECT_R)) {
        char c[1];
        if (read(signalpipe[0], c, 1) <= 0)
            /* ignore error */;
        if (sigint_pending)
            return;  /* putty_daemon_continue will return false */
    }

    /* Daemon mode: monitor stdin for Ctrl+C (0x03).
     * This is a fallback for when ISIG is off — if ISIG is on,
     * Ctrl+C generates SIGINT instead of being received as data. */
    if (daemon_mode && isatty(STDIN_FILENO) &&
        pollwrap_check_fd_rwx(pw, STDIN_FILENO, SELECT_R)) {
        char buf[32];
        int ret = read(STDIN_FILENO, buf, sizeof(buf));
        if (ret > 0) {
            for (int i = 0; i < ret; i++) {
                if ((unsigned char)buf[i] == 3) { /* Ctrl+C */
                    sigint_pending = true;
                    return;
                }
            }
        }
        /* Discard any other stdin input — daemon doesn't use it */
    }

    /* Accept new client connections */
    if (daemon_listen_fd >= 0 &&
        pollwrap_check_fd_rwx(pw, daemon_listen_fd, SELECT_R)) {
        /* If a previous client is still connected, close it first */
        if (daemon_client_fd >= 0) {
            close_daemon_client();
            daemon_client_input_done = false;
        }
        daemon_client_fd = accept_daemon_client(daemon_listen_fd);
        daemon_client_input_done = false;
    }

    /* Handle I/O with connected client */
    if (daemon_client_fd >= 0) {
        if (!daemon_client_input_done &&
            pollwrap_check_fd_rwx(pw, daemon_client_fd, SELECT_R)) {
            char buf[4096];
            int ret = read(daemon_client_fd, buf, sizeof(buf));
            if (ret > 0) {
                /* Buffer data; will be flushed to backend in pw_setup
                 * once backend is connected and ready to accept it. */
                bufchain_add(&daemon_client_input_buf, buf, ret);
            } else if (ret == 0) {
                /* EOF from client. Mark input as done but keep
                 * the fd open for writing response data back. */
                daemon_client_input_done = true;
                shutdown(daemon_client_fd, SHUT_RD);
            } else {
                fprintf(stderr, "putty: client read error: %s\n",
                        strerror(errno));
                close_daemon_client();
                return;
            }
        }

        if (daemon_client_fd >= 0 &&
            pollwrap_check_fd_rwx(pw, daemon_client_fd, SELECT_W)) {
            try_output(false);
            try_output(true);
        }
    }
}

static bool putty_daemon_continue(void *vctx, bool found_any_fd,
                                   bool ran_any_callback)
{
    /* Exit on SIGINT (Ctrl+C) */
    if (sigint_pending)
        return false;
    /* Keep running as long as SSH connection is alive */
    if (!backend_connected(backend))
        return false;
    return true;
}

/*
 * Client mode: connect to a daemon's Unix socket and relay stdin/stdout.
 * This is a standalone mode that does NOT create an SSH connection.
 */
static int run_client_mode(const char *socket_path, const char *command)
{
    struct sockaddr_un addr;
    int fd, ret, exitcode = 0;
    char buf[4096];
    fd_set rfds;
    bool stdin_done = false;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "putty: unable to create socket: %s\n",
                strerror(errno));
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0) {
        fprintf(stderr, "putty: unable to connect to daemon at '%s': %s\n",
                socket_path, strerror(errno));
        close(fd);
        return 1;
    }

    /* If a command was provided on the command line, send it immediately.
     * Do NOT call shutdown(SHUT_WR) here — doing so causes a race on
     * macOS Unix sockets where POLLHUP arrives before POLLIN, causing
     * the daemon to see EOF before reading the data, losing the command. */
    if (command && *command) {
        char *cmdline = dupprintf("%s\n", command);
        write(fd, cmdline, strlen(cmdline));
        sfree(cmdline);
        stdin_done = true;
    }

    /* Relay stdin <-> socket bidirectionally */
    bool saw_data = false;
    struct timeval start_time, last_data_time;
    bool start_time_set = false, last_data_set = false;
    while (1) {
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        int maxfd = fd;

        if (!stdin_done) {
            FD_SET(STDIN_FILENO, &rfds);
            if (STDIN_FILENO > maxfd)
                maxfd = STDIN_FILENO;
        }

        if (stdin_done) {
            /* After sending all input, use a timeout to avoid hanging.
             * Polling interval: 200ms. Exit when no data has arrived
             * for 500ms after the last received byte (or 15s total
             * if we never received anything). */
            struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
            ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);
            if (ret == 0) {
                if (saw_data && last_data_set) {
                    struct timeval now;
                    gettimeofday(&now, NULL);
                    long elapsed_ms = (now.tv_sec - last_data_time.tv_sec) * 1000
                        + (now.tv_usec - last_data_time.tv_usec) / 1000;
                    if (elapsed_ms >= 500) {
                        break;
                    }
                }
                /* Check total wait timeout for first byte */
                if (!start_time_set) {
                    gettimeofday(&start_time, NULL);
                    start_time_set = true;
                }
                struct timeval now;
                gettimeofday(&now, NULL);
                if (now.tv_sec - start_time.tv_sec > 15) {
                    fprintf(stderr, "putty: no response from daemon "
                            "(SSH connection may not be ready)\n");
                    exitcode = 1;
                    break;
                }
                continue;
            }
        } else {
            ret = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        }
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        /* Forward stdin data to daemon */
        if (!stdin_done && FD_ISSET(STDIN_FILENO, &rfds)) {
            ret = read(STDIN_FILENO, buf, sizeof(buf));
            if (ret > 0) {
                write(fd, buf, ret);
            } else {
                /* stdin EOF: shut down write side of socket */
                shutdown(fd, SHUT_WR);
                stdin_done = true;
            }
        }

        /* Forward daemon response to stdout */
        if (FD_ISSET(fd, &rfds)) {
            ret = read(fd, buf, sizeof(buf));
            if (ret > 0) {
                write(STDOUT_FILENO, buf, ret);
                saw_data = true;
                gettimeofday(&last_data_time, NULL);
                last_data_set = true;
            } else {
                fprintf(stderr, "putty-cli: daemon closed connection\n");
                break;
            }
        }
    }

    close(fd);
    return exitcode;
}

int main(int argc, char **argv)
{
    int exitcode;
    bool errors;
    enum TriState sanitise_stdout = AUTO, sanitise_stderr = AUTO;
    bool use_subsystem = false;
    bool just_test_share_exists = false;
    struct winsize size;
    const struct BackendVtable *backvt;

    enable_dit();

    /*
     * Initialise port and protocol to sensible defaults.
     */
    settings_set_default_protocol(PROT_SSH);
    settings_set_default_port(22);

    bufchain_init(&stdout_data);
    bufchain_init(&stderr_data);
    bufchain_sink_init(&stdout_bcs, &stdout_data);
    bufchain_sink_init(&stderr_bcs, &stderr_data);
    stdout_bs = BinarySink_UPCAST(&stdout_bcs);
    stderr_bs = BinarySink_UPCAST(&stderr_bcs);
    outgoingeof = EOF_NO;

    stderr_tty_init();
    /*
     * Process the command line.
     */
    conf = conf_new();
    do_defaults(NULL, conf);
    settings_set_default_protocol(conf_get_int(conf, CONF_protocol));
    settings_set_default_port(conf_get_int(conf, CONF_port));
    errors = false;
    {
        /*
         * Override the default protocol if PUTTY_PROTOCOL is set.
         */
        char *p = getenv("PUTTY_PROTOCOL");
        if (p) {
            const struct BackendVtable *vt = backend_vt_from_name(p);
            if (vt) {
                settings_set_default_protocol(vt->protocol);
                settings_set_default_port(vt->default_port);
                conf_set_int(conf, CONF_protocol, vt->protocol);
                conf_set_int(conf, CONF_port, vt->default_port);
            }
        }
    }
    CmdlineArgList *arglist = cmdline_arg_list_from_argv(argc, argv);
    size_t arglistpos = 0;
    while (arglist->args[arglistpos]) {
        CmdlineArg *arg = arglist->args[arglistpos++];
        CmdlineArg *nextarg = arglist->args[arglistpos];
        const char *p = cmdline_arg_to_str(arg);
        int ret = cmdline_process_param(arg, nextarg, 1, conf);
        if (ret == -2) {
            fprintf(stderr,
                    "putty: option \"%s\" requires an argument\n", p);
            errors = true;
        } else if (ret == 2) {
            arglistpos++;
        } else if (ret == 1) {
            continue;
        } else if (!strcmp(p, "-s")) {
            use_subsystem = true;
        } else if (!strcmp(p, "-V") || !strcmp(p, "--version")) {
            version();
        } else if (!strcmp(p, "--help")) {
            usage();
            exit(0);
        } else if (!strcmp(p, "-pgpfp")) {
            pgp_fingerprints();
            exit(0);
        } else if (!strcmp(p, "-shareexists")) {
            just_test_share_exists = true;
        } else if (!strcmp(p, "-sanitise-stdout") ||
                   !strcmp(p, "-sanitize-stdout")) {
            sanitise_stdout = FORCE_ON;
        } else if (!strcmp(p, "-no-sanitise-stdout") ||
                   !strcmp(p, "-no-sanitize-stdout")) {
            sanitise_stdout = FORCE_OFF;
        } else if (!strcmp(p, "-sanitise-stderr") ||
                   !strcmp(p, "-sanitize-stderr")) {
            sanitise_stderr = FORCE_ON;
        } else if (!strcmp(p, "-no-sanitise-stderr") ||
                   !strcmp(p, "-no-sanitize-stderr")) {
            sanitise_stderr = FORCE_OFF;
        } else if (!strcmp(p, "-no-antispoof")) {
            console_antispoof_prompt = false;
        } else if (!strcmp(p, "--daemon")) {
            if (nextarg) {
                daemon_mode = true;
                daemon_socket_path = dupstr(cmdline_arg_to_str(nextarg));
                arglistpos++;
            } else {
                fprintf(stderr, "putty: option \"--daemon\""
                        " requires a socket path argument\n");
                errors = true;
            }
        } else if (!strcmp(p, "--connect")) {
            if (nextarg) {
                client_mode = true;
                client_socket_path = dupstr(cmdline_arg_to_str(nextarg));
                arglistpos++;
            } else {
                fprintf(stderr, "putty: option \"--connect\""
                        " requires a socket path argument\n");
                errors = true;
            }
        } else if (*p != '-') {
            strbuf *cmdbuf = strbuf_new();

            while (arg) {
                if (cmdbuf->len > 0)
                    put_byte(cmdbuf, ' ');
                put_dataz(cmdbuf, cmdline_arg_to_str(arg));
                arg = arglist->args[arglistpos++];
            }

            conf_set_str(conf, CONF_remote_cmd, cmdbuf->s);
            conf_set_str(conf, CONF_remote_cmd2, "");
            conf_set_bool(conf, CONF_nopty, true);  /* command => no tty */

            strbuf_free(cmdbuf);
            break;                     /* done with cmdline */
        } else {
            fprintf(stderr, "putty: unknown option \"%s\"\n", p);
            errors = true;
        }
    }

    if (errors)
        return 1;

    /* If in client mode, skip SSH setup and connect to daemon */
    if (client_mode) {
        cmdline_arg_list_free(arglist);
        const char *cmd = conf_get_str_ambi(conf, CONF_remote_cmd, NULL);
        if (!cmd || !*cmd) {
            /* No explicit remote command found. But cmdline_process_param
             * may have stored a positional argument as CONF_host.
             * In client mode, positional arguments after --connect
             * are the command to execute, not a host name. */
            cmd = conf_get_str(conf, CONF_host);
            /* If it looks like user@host, strip it */
            if (cmd && *cmd) {
                const char *at = strrchr(cmd, '@');
                if (at) cmd = at + 1;
            }
        }
        return run_client_mode(client_socket_path,
                               (cmd && *cmd) ? cmd : NULL);
    }

    if (!cmdline_host_ok(conf)) {
        fprintf(stderr, "putty: no valid host name provided\n"
                "try \"putty --help\" for help\n");
        cmdline_arg_list_free(arglist);
        return 1;
    }

    prepare_session(conf);

    /*
     * Perform command-line overrides on session configuration.
     */
    cmdline_run_saved(conf);

    cmdline_arg_list_free(arglist);

    /*
     * If we have no better ideas for the remote username, use the local
     * one, as 'ssh' does.
     */
    if (conf_get_str_ambi(conf, CONF_username, NULL)[0] == '\0') {
        char *user = get_username();
        if (user) {
            conf_set_str(conf, CONF_username, user);
            sfree(user);
        }
    }

    /*
     * Apply subsystem status.
     */
    if (use_subsystem)
        conf_set_bool(conf, CONF_ssh_subsys, true);

    /*
     * Create daemon socket if in daemon mode.
     */
    if (daemon_mode) {
        daemon_listen_fd = create_daemon_socket(daemon_socket_path);
        if (daemon_listen_fd < 0)
            return 1;
        atexit(cleanup_daemon_socket);
        /* In daemon mode, skip interactive prompts */
        console_antispoof_prompt = false;
        /* No pty — avoids command echo and prompt noise in output,
         * and is the right default for programmatic command relay. */
        conf_set_bool(conf, CONF_nopty, true);
        bufchain_init(&daemon_client_input_buf);
    }

    /*
     * Select protocol.
     */
    backvt = backend_vt_from_proto(conf_get_int(conf, CONF_protocol));
    if (!backvt) {
        fprintf(stderr,
                "Internal fault: Unsupported protocol found\n");
        return 1;
    }

    if (backvt->flags & BACKEND_NEEDS_TERMINAL) {
        fprintf(stderr,
                "PuTTY CLI doesn't support %s, which needs terminal emulation\n",
                backvt->displayname_lc);
        return 1;
    }

    /*
     * Block SIGPIPE, so that we'll get EPIPE individually on
     * particular network connections that go wrong.
     */
    putty_signal(SIGPIPE, SIG_IGN);

    /*
     * Set up the pipe we'll use to tell us about SIGWINCH.
     */
    if (pipe(signalpipe) < 0) {
        perror("pipe");
        exit(1);
    }
    nonblock(signalpipe[0]);
    nonblock(signalpipe[1]);
    cloexec(signalpipe[0]);
    cloexec(signalpipe[1]);
    putty_signal(SIGWINCH, sigwinch);
    if (daemon_mode) {
        /* Install SIGINT handler WITHOUT SA_RESTART so that poll()
         * returns EINTR on Ctrl+C, instead of auto-restarting. */
        struct sigaction sa;
        sa.sa_handler = sigint;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;  /* no SA_RESTART! */
        sigaction(SIGINT, &sa, NULL);
    }

    /*
     * Now that we've got the SIGWINCH handler installed, try to find
     * out the initial terminal size.
     */
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) >= 0) {
        conf_set_int(conf, CONF_width, size.ws_col);
        conf_set_int(conf, CONF_height, size.ws_row);
    }

    /*
     * Decide whether to sanitise control sequences out of standard
     * output and standard error.
     */
    if (sanitise_stdout == FORCE_ON ||
        (sanitise_stdout == AUTO && isatty(STDOUT_FILENO) &&
         conf_get_bool(conf, CONF_nopty))) {
        stdout_scc = stripctrl_new(stdout_bs, true, L'\0');
        stdout_bs = BinarySink_UPCAST(stdout_scc);
    }
    if (sanitise_stderr == FORCE_ON ||
        (sanitise_stderr == AUTO && isatty(STDERR_FILENO) &&
         conf_get_bool(conf, CONF_nopty))) {
        stderr_scc = stripctrl_new(stderr_bs, true, L'\0');
        stderr_bs = BinarySink_UPCAST(stderr_scc);
    }

    sk_init();
    uxsel_init();

    /*
     * PuTTY CLI doesn't provide any way to add forwardings after the
     * connection is set up, so if there are none now, we can safely set
     * the "simple" flag.
     */
    if (conf_get_int(conf, CONF_protocol) == PROT_SSH &&
        !conf_get_bool(conf, CONF_x11_forward) &&
        !conf_get_bool(conf, CONF_agentfwd) &&
        !conf_get_str_nthstrkey(conf, CONF_portfwd, 0))
        conf_set_bool(conf, CONF_ssh_simple, true);

    if (just_test_share_exists) {
        if (!backvt->test_for_upstream) {
            fprintf(stderr, "Connection sharing not supported for this "
                    "connection type (%s)'\n", backvt->displayname_lc);
            return 1;
        }
        if (backvt->test_for_upstream(conf_get_str(conf, CONF_host),
                                      conf_get_int(conf, CONF_port), conf))
            return 0;
        else
            return 1;
    }

    /*
     * Start up the connection.
     */
    logctx = log_init(console_cli_logpolicy, conf);
    {
        char *error, *realhost;
        bool nodelay = conf_get_bool(conf, CONF_tcp_nodelay) && isatty(0);

#ifdef __AFL_HAVE_MANUAL_CONTROL
        __AFL_INIT();
#endif

        error = backend_init(backvt, putty_cli_seat, &backend, logctx, conf,
                             conf_get_str(conf, CONF_host),
                             conf_get_int(conf, CONF_port),
                             &realhost, nodelay,
                             conf_get_bool(conf, CONF_tcp_keepalives));
        if (error) {
            fprintf(stderr, "Unable to open connection:\n%s\n", error);
            sfree(error);
            return 1;
        }
        ldisc_create(conf, NULL, backend, putty_cli_seat);
        sfree(realhost);
    }

    /*
     * Set up the initial console mode.
     */
    local_tty = (tcgetattr(STDIN_FILENO, &orig_termios) == 0);
    /* In daemon mode, don't modify terminal settings — we want
     * Ctrl+C to keep working, and we don't read from stdin. */
    if (daemon_mode)
        local_tty = false;
    atexit(cleanup_termios);
    seat_echoedit_update(putty_cli_seat, 1, 1);

    if (daemon_mode)
        cli_main_loop(putty_daemon_pw_setup, putty_daemon_pw_check,
                      putty_daemon_continue, NULL);
    else
        cli_main_loop(putty_cli_pw_setup, putty_cli_pw_check,
                      putty_cli_continue, NULL);

    exitcode = backend_exitcode(backend);
    if (exitcode < 0) {
        fprintf(stderr, "Remote process exit code unavailable\n");
        exitcode = 1;
    }
    cleanup_exit(exitcode);
    return exitcode;
}
