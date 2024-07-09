/* associative array */
void assoc_init(const int hashpower_init);
item *assoc_find(const char *key, const size_t nkey, const uint32_t hv);
struct atlas_unique_ptr;
item *atlas_assoc_find_paging(const char *key, const size_t nkey, const uint32_t hv, struct atlas_unique_ptr **up);
int assoc_insert(item *item, const uint32_t hv);
int atlas_assoc_insert_get(item *it, const uint32_t hv, struct atlas_unique_ptr**up);
void assoc_delete(const char *key, const size_t nkey, const uint32_t hv);
void do_assoc_move_next_bucket(void);
int start_assoc_maintenance_thread(void);
void stop_assoc_maintenance_thread(void);
void assoc_start_expand(uint64_t curr_items);
/* walk functions */
void *assoc_get_iterator(void);
bool assoc_iterate(void *iterp, item **it);
void assoc_iterate_final(void *iterp);

extern unsigned int hashpower;
extern unsigned int item_lock_hashpower;
