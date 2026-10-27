// compile_check STAND-IN for the macOS SDK's <mach/mach_vm.h> (see
// ../../README.md): the MIG-generated interface of mach_vm.defs, which the
// Zig toolchain's Darwin headers leave out; the routines with the SDK's
// signatures (64-bit addresses and sizes, as <mach/vm_map.h> has them for
// vm_*).

#ifndef _mach_vm_user_
#define _mach_vm_user_

#include <mach/boolean.h>
#include <mach/kern_return.h>
#include <mach/mach_types.h>
#include <mach/message.h>
#include <mach/std_types.h>
#include <mach/vm_types.h>

#ifdef __cplusplus
extern "C" {
#endif

kern_return_t mach_vm_allocate(vm_map_t target,
                               mach_vm_address_t* address,
                               mach_vm_size_t size,
                               int flags);
kern_return_t mach_vm_deallocate(vm_map_t target,
                                 mach_vm_address_t address,
                                 mach_vm_size_t size);
kern_return_t mach_vm_protect(vm_map_t target_task,
                              mach_vm_address_t address,
                              mach_vm_size_t size,
                              boolean_t set_maximum,
                              vm_prot_t new_protection);
kern_return_t mach_vm_inherit(vm_map_t target_task,
                              mach_vm_address_t address,
                              mach_vm_size_t size,
                              vm_inherit_t new_inheritance);
kern_return_t mach_vm_read(vm_map_read_t target_task,
                           mach_vm_address_t address,
                           mach_vm_size_t size,
                           vm_offset_t* data,
                           mach_msg_type_number_t* dataCnt);
kern_return_t mach_vm_read_overwrite(vm_map_read_t target_task,
                                     mach_vm_address_t address,
                                     mach_vm_size_t size,
                                     mach_vm_address_t data,
                                     mach_vm_size_t* outsize);
kern_return_t mach_vm_write(vm_map_t target_task,
                            mach_vm_address_t address,
                            vm_offset_t data,
                            mach_msg_type_number_t dataCnt);
kern_return_t mach_vm_copy(vm_map_t target_task,
                           mach_vm_address_t source_address,
                           mach_vm_size_t size,
                           mach_vm_address_t dest_address);
kern_return_t mach_vm_region(vm_map_read_t target_task,
                             mach_vm_address_t* address,
                             mach_vm_size_t* size,
                             vm_region_flavor_t flavor,
                             vm_region_info_t info,
                             mach_msg_type_number_t* infoCnt,
                             mach_port_t* object_name);
kern_return_t mach_vm_region_recurse(vm_map_read_t target_task,
                                     mach_vm_address_t* address,
                                     mach_vm_size_t* size,
                                     natural_t* nesting_depth,
                                     vm_region_recurse_info_t info,
                                     mach_msg_type_number_t* infoCnt);
kern_return_t mach_vm_map(vm_map_t target_task,
                          mach_vm_address_t* address,
                          mach_vm_size_t size,
                          mach_vm_offset_t mask,
                          int flags,
                          mem_entry_name_port_t object,
                          memory_object_offset_t offset,
                          boolean_t copy,
                          vm_prot_t cur_protection,
                          vm_prot_t max_protection,
                          vm_inherit_t inheritance);
kern_return_t mach_vm_remap(vm_map_t target_task,
                            mach_vm_address_t* target_address,
                            mach_vm_size_t size,
                            mach_vm_offset_t mask,
                            int flags,
                            vm_map_t src_task,
                            mach_vm_address_t src_address,
                            boolean_t copy,
                            vm_prot_t* cur_protection,
                            vm_prot_t* max_protection,
                            vm_inherit_t inheritance);
kern_return_t mach_vm_purgable_control(vm_map_t target_task,
                                       mach_vm_address_t address,
                                       vm_purgable_t control,
                                       int* state);
kern_return_t mach_vm_behavior_set(vm_map_t target_task,
                                   mach_vm_address_t address,
                                   mach_vm_size_t size,
                                   vm_behavior_t new_behavior);
kern_return_t mach_vm_page_query(vm_map_read_t target_map,
                                 mach_vm_offset_t offset,
                                 integer_t* disposition,
                                 integer_t* ref_count);

#ifdef __cplusplus
}
#endif

#endif  // _mach_vm_user_
