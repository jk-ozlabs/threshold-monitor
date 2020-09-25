/**
 * Example application to monitor for threshold events on a
 * specific sensor, and invoke a host state change on threshold
 * assertion.
 */

#define _GNU_SOURCE

#include <err.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include <systemd/sd-bus.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))

static const char *prop_iface = "org.freedesktop.DBus.Properties";
static const char *propchange_member = "PropertiesChanged";
static const char *crit_iface = "xyz.openbmc_project.Sensor.Threshold.Critical";
static const char *crit_props[] = { "CriticalAlarmHigh", "CriticalAlarmLow" };

struct ctx {
	sd_bus	*bus;
};

static void handle_critical_threshold(struct ctx *ctx,
		const char *path, const char *prop)
{
	int rc;

	printf("Sensor %s asserted %s!\n", path, prop);

	/* Take appropriate action: in this example, we request a chassis
	 * state transition to Off */
	rc = sd_bus_call_method(ctx->bus,
			"xyz.openbmc_project.State.Chassis",
			"/xyz/openbmc_project/state/chassis0",
			prop_iface, "Set",
			NULL, NULL, "ssv",
			"xyz.openbmc_project.State.Chassis",
			"RequestedPowerTransition",
			"s",
			"xyz.openbmc_project.State.Chassis.Transition.Off");

	if (rc < 0)
		printf("failed to trigger host transition\n");
}

static bool is_threshold_prop(const char *propname)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(crit_props); i++) {
		if (!strcmp(crit_props[i], propname))
			return true;
	}

	return false;
}

/* Called by the sd_bus infrastructure when we see a message that matches
 * our addmatch filter. In this case, it should be a PropertiesChanged
 * signal on a Sensor Critical Thresholds interface. Check the changed
 * properties to determine if we need to take any action.
 */
static int propchange_handler(sd_bus_message *msg, void *data,
		sd_bus_error *errp)
{
	bool threshold_asserted;
	char *iface, *propname;
	struct ctx *ctx = data;
	int rc;

	(void)ctx;
	(void)errp;

	/* sanity checks on propchange event: ensure we have a
	 * signal, for a propchange event, on the crit_iface properties */
	if (!sd_bus_message_is_signal(msg, prop_iface, propchange_member))
		return 0;

	rc = sd_bus_message_read_basic(msg, 's', &iface);
	if (rc < 0)
		return rc;

	if (strcmp(iface, crit_iface))
		return 0;

	/* process changed properties, looking for the threshold states */
	rc = sd_bus_message_enter_container(msg, 'a', "{sv}");
	if (rc < 0)
		return rc;

	threshold_asserted = false;

	for (;;) {

		rc = sd_bus_message_enter_container(msg, 'e', "sv");
		if (rc < 0)
			break;

		rc = sd_bus_message_read(msg, "s", &propname);
		if (rc < 0)
			break;

		if (!is_threshold_prop(propname)) {
			sd_bus_message_skip(msg, "v");
		} else {
			int tmp;

			rc = sd_bus_message_read(msg, "v", "b", &tmp);
			if (rc < 0)
				break;

			if (tmp) {
				threshold_asserted = true;
				break;
			}
		}

		sd_bus_message_exit_container(msg);
	}

	if (threshold_asserted) {
		const char *sender;

		sender = sd_bus_message_get_path(msg);

		handle_critical_threshold(ctx, sender, propname);
	}

	return 0;
}

int main(void)
{
	struct ctx *ctx, _ctx;
	char *match;
	int rc;

	memset(&_ctx, 0, sizeof(_ctx));
	ctx = &_ctx;

	rc = sd_bus_default(&ctx->bus);
	if (rc < 0)
		errx(EXIT_FAILURE, "can't connect to dbus: %s", strerror(-rc));

	/* establish our match on the critial threshold interface */
	rc = asprintf(&match,
			"type='signal',interface='%s',member='%s',arg0='%s'",
			prop_iface, propchange_member, crit_iface);
	if (rc < 0)
		err(EXIT_FAILURE, "unable to define match");

	rc = sd_bus_add_match(ctx->bus, NULL, match, propchange_handler, ctx);
	if (rc < 0)
		errx(EXIT_FAILURE, "can't establish properties match: %s",
				strerror(-rc));

	/* core event loop: just process all dbus events */
	for (;;) {
		rc = sd_bus_process(ctx->bus, NULL);
		if (rc < 0) {
			warnx("can't process dbus events: %s", strerror(-rc));
			break;
		}

		/* no events? wait for the next */
		if (rc == 0)
			sd_bus_wait(ctx->bus, UINT64_MAX);
	}

	return EXIT_FAILURE;
}
