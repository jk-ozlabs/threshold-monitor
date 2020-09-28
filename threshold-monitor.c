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

enum threshold_type {
	T_HIGH = 0x1,
	T_LOW = 0x2,
};

struct sensor_config {
	const char	*path;
	int		thresholds;
};

/* Example configuration:
 *  - monitor for low and high events on Temp1
 *  - monitor only for high events on Temp2
 */
struct sensor_config sensor_configs[] = {
	{"/xyz/openbmc_project/sensors/temperature/Temp1", T_LOW | T_HIGH },
	{"/xyz/openbmc_project/sensors/temperature/Temp2", T_HIGH },
};

static const char *prop_iface = "org.freedesktop.DBus.Properties";
static const char *propchange_member = "PropertiesChanged";
static const char *crit_iface = "xyz.openbmc_project.Sensor.Threshold.Critical";

/* mapping of configuration config values to property names */
struct {
	int		config;
	const char	*prop;
} prop_names[] = {
	{ T_HIGH, "CriticalAlarmHigh" },
	{ T_LOW, "CriticalAlarmLow" },
};

struct ctx {
	sd_bus	*bus;
};

static bool threshold_prop_matches_config(const struct sensor_config *config,
		const char *propname)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(prop_names); i++) {
		if (!strcmp(prop_names[i].prop, propname))
			return config->thresholds & prop_names[i].config;
	}

	return false;
}

static const struct sensor_config *find_sensor_config(const char *path)
{
	unsigned int i;

	if (!path)
		return NULL;

	for (i = 0; i < ARRAY_SIZE(sensor_configs); i++) {
		struct sensor_config *cfg = &sensor_configs[i];
		if (!strcmp(cfg->path, path))
			return cfg;
	}

	return NULL;
}

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

/* Called by the sd_bus infrastructure when we see a message that matches
 * our addmatch filter. In this case, it should be a PropertiesChanged
 * signal on a Sensor Critical Thresholds interface. Check the changed
 * properties to determine if we need to take any action.
 */
static int propchange_handler(sd_bus_message *msg, void *data,
		sd_bus_error *errp)
{
	const struct sensor_config *sensor;
	bool threshold_asserted;
	char *iface, *propname;
	struct ctx *ctx = data;
	const char *sender;
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

	/* is this from a sensor that we are listening for? */
	sender = sd_bus_message_get_path(msg);
	sensor = find_sensor_config(sender);
	if (!sensor)
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

		if (!threshold_prop_matches_config(sensor, propname)) {
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
