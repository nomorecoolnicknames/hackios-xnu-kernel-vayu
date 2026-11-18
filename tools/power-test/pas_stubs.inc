static bool glink_say(struct vayu_glink *g, uint16_t cmd, uint16_t channel, uint32_t param,
    const void *data, uint32_t size)
{ (void)g; (void)cmd; (void)channel; (void)param; (void)data; (void)size; return true; }
static bool glink_offer(struct vayu_glink *g, struct vayu_glink_channel *c, uint32_t n)
{ (void)g; (void)c; (void)n; return false; }
static const char *glink_name(uint16_t cmd) { (void)cmd; return "test"; }
