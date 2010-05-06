/* This is a master for the "distributed" engine. It receives connections
 * from slave machines, sends them gtp commands, then aggregates the
 * results. It can also act as a proxy for the logs of all slave machines.
 * The slave machines must run with engine "uct" (not "distributed").
 * The master sends pachi-genmoves gtp commands regularly to each slave,
 * gets as replies a list of candidate moves, their number of playouts
 * and their value. The master then picks the most popular move. */

/* With time control, the master waits for all slaves, except
 * when the allowed time is already passed. In this case the
 * master picks among the available replies, or waits for just
 * one reply if there is none yet.
 * Without time control, the master waits until the desired
 * number of games have been simulated. In this case the -t
 * parameter for the master should be the sum of the parameters
 * for all slaves. */

/* The master sends updated statistics for the best moves
 * in each genmoves command. In this version only the
 * children of the root node are updated. The slaves
 * reply with just their own stats; they remember what was
 * previously received from or sent to the master, to
 * distinguish their own contribution from that of other slaves. */

/* The master-slave protocol has has fault tolerance. If a slave is
 * out of sync, the master sends it the appropriate command history. */

/* Pass me arguments like a=b,c=d,...
 * Supported arguments:
 * slave_port=SLAVE_PORT  slaves connect to this port; this parameter is mandatory.
 * max_slaves=MAX_SLAVES  default 100
 * slaves_quit=0|1        quit gtp command also sent to slaves, default false.
 * proxy_port=PROXY_PORT  slaves optionally send their logs to this port.
 *    Warning: with proxy_port, the master stderr mixes the logs of all
 *    machines but you can separate them again:
 *      slave logs:  sed -n '/< .*:/s/.*< /< /p' logfile
 *      master logs: perl -0777 -pe 's/<[ <].*:.*\n//g' logfile
 */

/* A configuration without proxy would have one master run on masterhost as:
 *    zzgo -e distributed slave_port=1234
 * and N slaves running as:
 *    zzgo -e uct -g masterhost:1234 slave
 * With log proxy:
 *    zzgo -e distributed slave_port=1234,proxy_port=1235
 *    zzgo -e uct -g masterhost:1234 -l masterhost:1235 slave
 * If the master itself runs on a machine other than that running gogui,
 * gogui-twogtp, kgsGtp or cgosGtp, it can redirect its gtp port:
 *    zzgo -e distributed -g 10000 slave_port=1234,proxy_port=1235
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <alloca.h>
#include <sys/types.h>

#define DEBUG

#include "engine.h"
#include "move.h"
#include "timeinfo.h"
#include "playout.h"
#include "stats.h"
#include "mq.h"
#include "debug.h"
#include "distributed/distributed.h"
#include "distributed/protocol.h"

/* Internal engine state. */
struct distributed {
	char *slave_port;
	char *proxy_port;
	int max_slaves;
	bool slaves_quit;
	struct move my_last_move;
	struct move_stats my_last_stats;
};

/* Default number of simulations to perform per move.
 * Note that this is in total over all slaves! */
#define DIST_GAMES	80000
static const struct time_info default_ti = {
	.period = TT_MOVE,
	.dim = TD_GAMES,
	.len = { .games = DIST_GAMES },
};

#define get_value(value, color) \
	((color) == S_BLACK ? (value) : 1 - (value))


/* Maximum time (seconds) to wait for answers to fast gtp commands
 * (all commands except pachi-genmoves and final_status_list). */
#define MAX_FAST_CMD_WAIT 1.0

/* How often to send a stats update to slaves (seconds) */
#define STATS_UPDATE_INTERVAL 0.1 /* 100ms */

/* Maximum time (seconds) to wait between genmoves
 * (all commands except pachi-genmoves and final_status_list). */
#define MAX_FAST_CMD_WAIT 1.0

/* Dispatch a new gtp command to all slaves.
 * The slave lock must not be held upon entry and is released upon return.
 * args is empty or ends with '\n' */
static enum parse_code
distributed_notify(struct engine *e, struct board *b, int id, char *cmd, char *args, char **reply)
{
	struct distributed *dist = e->data;

	/* Commands that should not be sent to slaves.
	 * time_left will be part of next pachi-genmoves,
	 * we reduce latency by not forwarding it here. */
	if ((!strcasecmp(cmd, "quit") && !dist->slaves_quit)
	    || !strcasecmp(cmd, "uct_genbook")
	    || !strcasecmp(cmd, "uct_dumpbook")
	    || !strcasecmp(cmd, "kgs-chat")
	    || !strcasecmp(cmd, "time_left")

	    /* and commands that will be sent to slaves later */
	    || !strcasecmp(cmd, "genmove")
	    || !strcasecmp(cmd, "kgs-genmove_cleanup")
	    || !strcasecmp(cmd, "final_score")
	    || !strcasecmp(cmd, "final_status_list"))
		return P_OK;

	protocol_lock();

	// Create a new command to be sent by the slave threads.
	new_cmd(b, cmd, args);

	/* Wait for replies here. If we don't wait, we run the
	 * risk of getting out of sync with most slaves and
	 * sending command history too frequently. */
	get_replies(time_now() + MAX_FAST_CMD_WAIT);

	protocol_unlock();
	return P_OK;
}

/* genmoves returns a line "=id played_own total_playouts threads keep_looking[ reserved]"
 * then a list of lines "coord playouts value amaf_playouts amaf_value".
 * Return the move with most playouts, and additional stats.
 * Keep this code in sync with uct/slave.c:report_stats().
 * slave_lock is held on entry and on return. */
static coord_t
select_best_move(struct board *b, struct move_stats2 *stats, int *played,
		 int *total_playouts, int *total_threads, bool *keep_looking)
{
	assert(reply_count > 0);

	/* +2 for pass and resign */
	memset(stats-2, 0, (board_size2(b)+2) * sizeof(*stats));

	coord_t best_move = pass;
	int best_playouts = -1;
	*played = 0;
	*total_playouts = 0;
	*total_threads = 0;
	int keep = 0;

	for (int reply = 0; reply < reply_count; reply++) {
		char *r = gtp_replies[reply];
		int id, o, p, t, k;
		if (sscanf(r, "=%d %d %d %d %d", &id, &o, &p, &t, &k) != 5) continue;
		*played += o;
		*total_playouts += p;
		*total_threads += t;
		keep += k;
		// Skip the rest of the firt line if any (allow future extensions)
		r = strchr(r, '\n');

		char move[64];
		struct move_stats2 s;
		while (r && sscanf(++r, "%63s %d %f %d %f", move, &s.u.playouts,
				   &s.u.value, &s.amaf.playouts, &s.amaf.value) == 5) {
			coord_t *c = str2coord(move, board_size(b));
			stats_add_result(&stats[*c].u, s.u.value, s.u.playouts);
			stats_add_result(&stats[*c].amaf, s.amaf.value, s.amaf.playouts);

			if (stats[*c].u.playouts > best_playouts) {
				best_playouts = stats[*c].u.playouts;
				best_move = *c;
			}
			coord_done(c);
			r = strchr(r, '\n');
		}
	}
	*keep_looking = keep > reply_count / 2;
	return best_move;
}

/* Set the args for the genmoves command. If stats is not null,
 * append the stats from all slaves above min_playouts, except
 * for pass and resign. args must have CMDS_SIZE bytes and
 * upon return ends with an empty line.
 * Keep this code in sync with uct_genmoves().
 * slave_lock is held on entry and on return. */
static void
genmoves_args(char *args, struct board *b, enum stone color, int played,
	      struct time_info *ti, struct move_stats2 *stats, int min_playouts)
{
	char *end = args + CMDS_SIZE;
	char *s = args + snprintf(args, CMDS_SIZE, "%s %d", stone2str(color), played);

	if (ti->dim == TD_WALLTIME) {
		s += snprintf(s, end - s, " %.3f %.3f %d %d",
			      ti->len.t.main_time, ti->len.t.byoyomi_time,
			      ti->len.t.byoyomi_periods, ti->len.t.byoyomi_stones);
	}
	s += snprintf(s, end - s, "\n");
	if (stats) {
		foreach_point(b) {
			if (stats[c].u.playouts <= min_playouts) continue;
			s += snprintf(s, end - s, "%s %d %.7f %d %.7f\n",
				      coord2sstr(c, b),
				      stats[c].u.playouts, stats[c].u.value,
				      stats[c].amaf.playouts, stats[c].amaf.value);
		} foreach_point_end;
	}
	s += snprintf(s, end - s, "\n");
}

/* Time control is mostly done by the slaves, so we use default values here. */
#define FUSEKI_END 20
#define YOSE_START 40

static coord_t *
distributed_genmove(struct engine *e, struct board *b, struct time_info *ti,
		    enum stone color, bool pass_all_alive)
{
	struct distributed *dist = e->data;
	double now = time_now();
	double first = now;

	char *cmd = pass_all_alive ? "pachi-genmoves_cleanup" : "pachi-genmoves";
	char args[CMDS_SIZE];

	coord_t best;
	int played, playouts, threads;

	if (ti->period == TT_NULL) *ti = default_ti;
	struct time_stop stop;
	time_stop_conditions(ti, b, FUSEKI_END, YOSE_START, &stop);
	struct time_info saved_ti = *ti;

	/* Send the first genmoves without stats. */
	genmoves_args(args, b, color, 0, ti, NULL, 0);

	/* Combined move stats from all slaves, only for children
	 * of the root node, plus 2 for pass and resign. */
	struct move_stats2 *stats = alloca((board_size2(b)+2) * sizeof(struct move_stats2));
	stats += 2;

	protocol_lock();
	new_cmd(b, cmd, args);

	/* Loop until most slaves want to quit or time elapsed. */
	for (;;) {
		double start = now;
		get_replies(now + STATS_UPDATE_INTERVAL);
		now = time_now();
		if (ti->dim == TD_WALLTIME)
			time_sub(ti, now - start, false);

		bool keep_looking;
		best = select_best_move(b, stats, &played, &playouts, &threads, &keep_looking);

		if (!keep_looking) break;
		if (ti->dim == TD_WALLTIME) {
			if (now - ti->len.t.timer_start >= stop.worst.time) break;
		} else {
			if (played >= stop.worst.playouts) break;
		}
		if (DEBUGL(2)) {
			char buf[BSIZE];
			char *coord = coord2sstr(best, b);
			snprintf(buf, sizeof(buf),
				 "temp winner is %s %s with score %1.4f (%d/%d games)"
				 " %d slaves %d threads\n",
				 stone2str(color), coord, get_value(stats[best].u.value, color),
				 stats[best].u.playouts, playouts, reply_count, threads);
			logline(NULL, "* ", buf);
		}
		/* Send the command with the same gtp id, to avoid discarding
		 * a reply to a previous genmoves at the same move. */
		genmoves_args(args, b, color, played, ti, stats, stats[best].u.playouts / 100);
		update_cmd(b, cmd, args, false);
	}
	int replies = reply_count;

	/* Do not subtract time spent twice (see gtp_parse). */
	*ti = saved_ti;

	dist->my_last_move.color = color;
	dist->my_last_move.coord = best;
	dist->my_last_stats = stats[best].u;

	/* Tell the slaves to commit to the selected move, overwriting
	 * the last "pachi-genmoves" in the command history. */
	char *coord = coord2str(best, b);
	snprintf(args, sizeof(args), "%s %s\n", stone2str(color), coord);
	update_cmd(b, "play", args, true);
	protocol_unlock();

	if (DEBUGL(1)) {
		char buf[BSIZE];
		double time = now - first + 0.000001; /* avoid divide by zero */
		snprintf(buf, sizeof(buf),
			 "GLOBAL WINNER is %s %s with score %1.4f (%d/%d games)\n"
			 "genmove %d games in %0.2fs %d slaves %d threads (%d games/s,"
			 " %d games/s/slave, %d games/s/thread)\n",
			 stone2str(color), coord, get_value(stats[best].u.value, color),
			 stats[best].u.playouts, playouts, played, time, replies, threads,
			 (int)(played/time), (int)(played/time/replies),
			 (int)(played/time/threads));
		logline(NULL, "* ", buf);
	}
	free(coord);
	return coord_copy(best);
}

static char *
distributed_chat(struct engine *e, struct board *b, char *cmd)
{
	struct distributed *dist = e->data;
	static char reply[BSIZE];

	cmd += strspn(cmd, " \n\t");
	if (!strncasecmp(cmd, "winrate", 7)) {
		enum stone color = dist->my_last_move.color;
		snprintf(reply, BSIZE, "In %d playouts at %d machines, %s %s can win with %.2f%% probability.",
			 dist->my_last_stats.playouts, active_slaves, stone2str(color),
			 coord2sstr(dist->my_last_move.coord, b),
			 100 * get_value(dist->my_last_stats.value, color));
		return reply;
	}
	return NULL;
}

static int
scmp(const void *p1, const void *p2)
{
	return strcasecmp(*(char * const *)p1, *(char * const *)p2);
}

static void
distributed_dead_group_list(struct engine *e, struct board *b, struct move_queue *mq)
{
	protocol_lock();

	new_cmd(b, "final_status_list", "dead\n");
	get_replies(time_now() + MAX_FAST_CMD_WAIT);

	/* Find the most popular reply. */
	qsort(gtp_replies, reply_count, sizeof(char *), scmp);
	int best_reply = 0;
	int best_count = 1;
	int count = 1;
	for (int reply = 1; reply < reply_count; reply++) {
		if (!strcmp(gtp_replies[reply], gtp_replies[reply-1])) {
			count++;
		} else {
			count = 1;
		}
		if (count > best_count) {
			best_count = count;
			best_reply = reply;
		}
	}

	/* Pick the first move of each line as group. */
	char *dead = gtp_replies[best_reply];
	dead = strchr(dead, ' '); // skip "id "
	while (dead && *++dead != '\n') {
		coord_t *c = str2coord(dead, board_size(b));
		mq_add(mq, *c);
		coord_done(c);
		dead = strchr(dead, '\n');
	}
	protocol_unlock();
}

static struct distributed *
distributed_state_init(char *arg, struct board *b)
{
	struct distributed *dist = calloc2(1, sizeof(struct distributed));

	dist->max_slaves = 100;
	if (arg) {
		char *optspec, *next = arg;
		while (*next) {
			optspec = next;
			next += strcspn(next, ",");
			if (*next) { *next++ = 0; } else { *next = 0; }

			char *optname = optspec;
			char *optval = strchr(optspec, '=');
			if (optval) *optval++ = 0;

			if (!strcasecmp(optname, "slave_port") && optval) {
				dist->slave_port = strdup(optval);
			} else if (!strcasecmp(optname, "proxy_port") && optval) {
				dist->proxy_port = strdup(optval);
			} else if (!strcasecmp(optname, "max_slaves") && optval) {
				dist->max_slaves = atoi(optval);
			} else if (!strcasecmp(optname, "slaves_quit")) {
				dist->slaves_quit = !optval || atoi(optval);
			} else {
				fprintf(stderr, "distributed: Invalid engine argument %s or missing value\n", optname);
			}
		}
	}

	gtp_replies = calloc2(dist->max_slaves, sizeof(char *));

	if (!dist->slave_port) {
		fprintf(stderr, "distributed: missing slave_port\n");
		exit(1);
	}
	protocol_init(dist->slave_port, dist->proxy_port, dist->max_slaves);
	return dist;
}

struct engine *
engine_distributed_init(char *arg, struct board *b)
{
	struct distributed *dist = distributed_state_init(arg, b);
	struct engine *e = calloc2(1, sizeof(struct engine));
	e->name = "Distributed Engine";
	e->comment = "I'm playing the distributed engine. When I'm losing, I will resign, "
		"if I think I win, I play until you pass. "
		"Anyone can send me 'winrate' in private chat to get my assessment of the position.";
	e->notify = distributed_notify;
	e->genmove = distributed_genmove;
	e->dead_group_list = distributed_dead_group_list;
	e->chat = distributed_chat;
	e->data = dist;
	// Keep the threads and the open socket connections:
	e->keep_on_clear = true;

	return e;
}
