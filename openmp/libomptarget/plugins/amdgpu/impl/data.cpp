/*===--------------------------------------------------------------------------
 *              ATMI (Asynchronous Task and Memory Interface)
 *
 * This file is distributed under the MIT License. See LICENSE.txt for details.
 *===------------------------------------------------------------------------*/
#include "data.h"
#include "atmi_runtime.h"
#include "internal.h"
#include "machine.h"
#include "rt.h"
#include <cassert>
#include <hsa.h>
#include <hsa_ext_amd.h>
#include <iostream>
#include <stdio.h>
#include <string.h>
#include <thread>
#include <vector>

using core::TaskImpl;
extern ATLMachine g_atl_machine;
extern hsa_signal_t IdentityCopySignal;

namespace core {
ATLPointerTracker g_data_map; // Track all am pointer allocations.
void allow_access_to_all_gpu_agents(void *ptr);

const char *getPlaceStr(atmi_devtype_t type) {
  switch (type) {
  case ATMI_DEVTYPE_CPU:
    return "CPU";
  case ATMI_DEVTYPE_GPU:
    return "GPU";
  default:
    return NULL;
  }
}

std::ostream &operator<<(std::ostream &os, const ATLData *ap) {
  atmi_mem_place_t place = ap->place();
  os << " devicePointer:" << ap->ptr() << " sizeBytes:" << ap->size()
     << " place:(" << getPlaceStr(place.dev_type) << ", " << place.dev_id
     << ", " << place.mem_id << ")";
  return os;
}

void ATLPointerTracker::insert(void *pointer, ATLData *p) {
  std::lock_guard<std::mutex> l(mutex_);

  DEBUG_PRINT("insert: %p + %zu\n", pointer, p->size());
  tracker_.insert(std::make_pair(ATLMemoryRange(pointer, p->size()), p));
}

void ATLPointerTracker::remove(void *pointer) {
  std::lock_guard<std::mutex> l(mutex_);
  DEBUG_PRINT("remove: %p\n", pointer);
  tracker_.erase(ATLMemoryRange(pointer, 1));
}

ATLData *ATLPointerTracker::find(const void *pointer) {
  std::lock_guard<std::mutex> l(mutex_);
  ATLData *ret = NULL;
  auto iter = tracker_.find(ATLMemoryRange(pointer, 1));
  DEBUG_PRINT("find: %p\n", pointer);
  if (iter != tracker_.end()) // found
    ret = iter->second;
  return ret;
}

ATLProcessor &get_processor_by_mem_place(atmi_mem_place_t place) {
  int dev_id = place.dev_id;
  switch (place.dev_type) {
  case ATMI_DEVTYPE_CPU:
    return g_atl_machine.processors<ATLCPUProcessor>()[dev_id];
  case ATMI_DEVTYPE_GPU:
    return g_atl_machine.processors<ATLGPUProcessor>()[dev_id];
  }
}

static hsa_agent_t get_mem_agent(atmi_mem_place_t place) {
  return get_processor_by_mem_place(place).agent();
}

hsa_amd_memory_pool_t get_memory_pool_by_mem_place(atmi_mem_place_t place) {
  ATLProcessor &proc = get_processor_by_mem_place(place);
  return get_memory_pool(proc, place.mem_id);
}

void register_allocation(void *ptr, size_t size, atmi_mem_place_t place) {
  ATLData *data = new ATLData(ptr, size, place);
  g_data_map.insert(ptr, data);
  if (place.dev_type == ATMI_DEVTYPE_CPU)
    allow_access_to_all_gpu_agents(ptr);
  // TODO(ashwinma): what if one GPU wants to access another GPU?
}

atmi_status_t Runtime::Malloc(void **ptr, size_t size, atmi_mem_place_t place) {
  atmi_status_t ret = ATMI_STATUS_SUCCESS;
  hsa_amd_memory_pool_t pool = get_memory_pool_by_mem_place(place);
  hsa_status_t err = hsa_amd_memory_pool_allocate(pool, size, 0, ptr);
  ErrorCheck(atmi_malloc, err);
  DEBUG_PRINT("Malloced [%s %d] %p\n",
              place.dev_type == ATMI_DEVTYPE_CPU ? "CPU" : "GPU", place.dev_id,
              *ptr);
  if (err != HSA_STATUS_SUCCESS)
    ret = ATMI_STATUS_ERROR;

  register_allocation(*ptr, size, place);

  return ret;
}

atmi_status_t Runtime::Memfree(void *ptr) {
  atmi_status_t ret = ATMI_STATUS_SUCCESS;
  hsa_status_t err;
  ATLData *data = g_data_map.find(ptr);
  if (!data)
    ErrorCheck(Checking pointer info userData,
               HSA_STATUS_ERROR_INVALID_ALLOCATION);

  g_data_map.remove(ptr);
  delete data;

  err = hsa_amd_memory_pool_free(ptr);
  ErrorCheck(atmi_free, err);
  DEBUG_PRINT("Freed %p\n", ptr);

  if (err != HSA_STATUS_SUCCESS || !data)
    ret = ATMI_STATUS_ERROR;
  return ret;
}

static hsa_status_t invoke_hsa_copy(void *dest, const void *src, size_t size,
                                    hsa_agent_t agent) {
  // TODO: Use thread safe signal
  hsa_signal_store_release(IdentityCopySignal, 1);

  hsa_status_t err = hsa_amd_memory_async_copy(dest, agent, src, agent, size, 0,
                                               NULL, IdentityCopySignal);
  ErrorCheck(Copy async between memory pools, err);

  // TODO: async reports errors in the signal, use NE 1
  hsa_signal_wait_acquire(IdentityCopySignal, HSA_SIGNAL_CONDITION_EQ, 0,
                          UINT64_MAX, ATMI_WAIT_STATE);

  return err;
}

atmi_status_t Runtime::Memcpy(void *dest, const void *src, size_t size) {
  atmi_status_t ret;
  hsa_status_t err;
  ATLData *src_data = g_data_map.find(src);
  ATLData *dest_data = g_data_map.find(dest);
  atmi_mem_place_t cpu = ATMI_MEM_PLACE_CPU_MEM(0, 0, 0);
  void *temp_host_ptr;

  if (src_data && !dest_data) {
    // Copy from device to scratch to host
    hsa_agent_t agent = get_mem_agent(src_data->place());
    DEBUG_PRINT("Memcpy D2H device agent: %lu\n", agent.handle);
    ret = atmi_malloc(&temp_host_ptr, size, cpu);
    if (ret != ATMI_STATUS_SUCCESS) {
      return ret;
    }

    err = invoke_hsa_copy(temp_host_ptr, src, size, agent);
    if (err != HSA_STATUS_SUCCESS) {
      return ATMI_STATUS_ERROR;
    }

    memcpy(dest, temp_host_ptr, size);

  } else if (!src_data && dest_data) {
    // Copy from host to scratch to device
    hsa_agent_t agent = get_mem_agent(dest_data->place());
    DEBUG_PRINT("Memcpy H2D device agent: %lu\n", agent.handle);
    ret = atmi_malloc(&temp_host_ptr, size, cpu);
    if (ret != ATMI_STATUS_SUCCESS) {
      return ret;
    }

    memcpy(temp_host_ptr, src, size);

    DEBUG_PRINT("Memcpy device agent: %lu\n", agent.handle);
    err = invoke_hsa_copy(dest, temp_host_ptr, size, agent);

  } else if (!src_data && !dest_data) {
    DEBUG_PRINT("atmi_memcpy invoked without metadata\n");
    // would be host to host, just call memcpy, or missing metadata
    return ATMI_STATUS_ERROR;
  } else {
    DEBUG_PRINT("atmi_memcpy unimplemented device to device copy\n");
    return ATMI_STATUS_ERROR;
  }

  ret = atmi_free(temp_host_ptr);

  if (err != HSA_STATUS_SUCCESS || ret != ATMI_STATUS_SUCCESS)
    ret = ATMI_STATUS_ERROR;
  return ret;
}

} // namespace core
