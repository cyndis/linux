
struct nouveau_object;
struct nouveau_gpuobj;
struct nouveau_channel;

void dump_pte(u32 ptepfn, u32 base);
void dump_pgt(u32 pgtpfn);
void dump_ramuser(struct nouveau_channel *chan);
void dump_ramin(struct nouveau_channel *chan, u32 raminpfn);
void dump_channel(struct nouveau_channel *chan);
/*void dump_runlist(struct nouveau_object *priv, u32 nchan, struct nouveau_gpuobj *cur);*/