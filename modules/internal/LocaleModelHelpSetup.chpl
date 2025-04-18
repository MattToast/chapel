/*
 * Copyright 2017 Advanced Micro Devices, Inc.
 * Copyright 2020-2025 Hewlett Packard Enterprise Development LP
 * Copyright 2004-2019 Cray Inc.
 * Other additional copyright holders may be indicated within.
 *
 * The entirety of this work is licensed under the Apache License,
 * Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// LocaleModelHelpSetup.chpl
//
// Provides for declarations common to locale model setup
// but that do not have to be the same in order to meet the
// interface.

// They are in this file as a practical matter to avoid code
// duplication. If necessary, a locale model using this file
// should feel free to reimplement them in some other way.
module LocaleModelHelpSetup {

  use ChapelLocale;
  use DefaultRectangular;
  use ChapelNumLocales;
  use OS.POSIX;
  use CTypes;

  config param debugLocaleModel = false;

  pragma "fn synchronization free"
  extern "get_chpl_nodeID" proc chpl_nodeID: chpl_nodeID_t;

  record chpl_root_locale_accum {
    var nPUsPhysAcc: atomic int;
    var nPUsPhysAll: atomic int;
    var nPUsLogAcc: atomic int;
    var nPUsLogAll: atomic int;
    var maxTaskPar: atomic int;

    // override compiler-generated default initializer for now because
    // we don't rely on it, and it generates a --warn-unstable error
    // for the time being (due to taking 'atomic int' formals rather
    // than 'int' formals)
    proc init() {
    }
    proc init=(other: chpl_root_locale_accum) {
      init this;
      this.nPUsPhysAcc.write(other.nPUsPhysAcc.read());
      this.nPUsPhysAll.write(other.nPUsPhysAll.read());
      this.nPUsLogAcc.write(other.nPUsLogAcc.read());
      this.nPUsLogAll.write(other.nPUsLogAll.read());
      this.maxTaskPar.write(other.maxTaskPar.read());
    }

    proc ref accum(loc:locale) {
      nPUsPhysAcc.add(loc.nPUsPhysAcc);
      nPUsPhysAll.add(loc.nPUsPhysAll);
      nPUsLogAcc.add(loc.nPUsLogAcc);
      nPUsLogAll.add(loc.nPUsLogAll);
      maxTaskPar.add(loc.maxTaskPar);
    }
    proc setRootLocaleValues(dst:borrowed RootLocale) {
      dst.nPUsPhysAcc = nPUsPhysAcc.read();
      dst.nPUsPhysAll = nPUsPhysAll.read();
      dst.nPUsLogAcc = nPUsLogAcc.read();
      dst.nPUsLogAll = nPUsLogAll.read();
      dst.maxTaskPar = maxTaskPar.read();
    }
  }

  proc helpSetupRootLocaleFlat(dst:borrowed RootLocale) {
    var root_accum:chpl_root_locale_accum;

    forall locIdx in dst.chpl_initOnLocales() with (ref root_accum) {
      const node = new locale(new unmanaged LocaleModel(new locale(dst)));
      dst.myLocales[locIdx] = node;
      root_accum.accum(node);
    }

    root_accum.setRootLocaleValues(dst);
  }

  proc helpSetupRootLocaleNUMA(dst:borrowed RootLocale) {
    extern proc chpl_task_setSubloc(subloc: int(32));

    var root_accum:chpl_root_locale_accum;

    forall locIdx in dst.chpl_initOnLocales() with (ref root_accum) {
      chpl_task_setSubloc(c_sublocid_none);
      const node = new locale(new unmanaged LocaleModel(new locale (dst)));
      dst.myLocales[locIdx] = node;
      root_accum.accum(node);
    }

    root_accum.setRootLocaleValues(dst);
  }

  proc helpSetupRootLocaleGPU(dst:borrowed RootLocale) {
    extern proc chpl_task_setSubloc(subloc: int(32));

    var root_accum:chpl_root_locale_accum;

    forall locIdx in dst.chpl_initOnLocales() with (ref root_accum) {
      chpl_task_setSubloc(c_sublocid_none);
      const node = new locale(new unmanaged LocaleModel(new locale (dst)));
      dst.myLocales[locIdx] = node;
      root_accum.accum(node);
    }

    root_accum.setRootLocaleValues(dst);
  }

  // gasnet-smp and gasnet-udp w/ GASNET_SPAWNFN=L are local spawns
  private inline proc localSpawn() {
    use ChplConfig;
    if CHPL_COMM == "gasnet" {
      if CHPL_COMM_SUBSTRATE == "udp" {
        try! {
          const spawnfn = getenv("GASNET_SPAWNFN");
          const spawnfnS = string.createBorrowingBuffer(spawnfn);
          if spawnfn != nil && spawnfnS == "L" {
            return true;
          }
        }
      } else if (CHPL_COMM_SUBSTRATE == "smp") {
        return true;
      }
    }
    return false;
  }

  private inline proc getNodeName() {
    // chpl_nodeName is defined in chplsys.c.
    // It supplies a node name obtained by running uname(3) on the
    // current node.  For this reason (as well), the constructor (or
    // at least this setup method) must be run on the node it is
    // intended to describe.
    extern proc chpl_nodeName(): c_ptrConst(c_char);
    var _node_name: string;
    try! {
      _node_name = string.createCopyingBuffer(chpl_nodeName());
    }
    const _node_id = (chpl_nodeID: int): string;

    return if localSpawn() then _node_name + "-" + _node_id else _node_name;
  }

  proc helpSetupLocaleFlat(dst:borrowed LocaleModel, out local_name:string) {
    local_name = getNodeName();

    extern proc chpl_topo_getNumCPUsPhysical(accessible_only: bool): c_int;
    dst.nPUsPhysAcc = chpl_topo_getNumCPUsPhysical(true);
    dst.nPUsPhysAll = chpl_topo_getNumCPUsPhysical(false);

    extern proc chpl_topo_getNumCPUsLogical(accessible_only: bool): c_int;
    dst.nPUsLogAcc = chpl_topo_getNumCPUsLogical(true);
    dst.nPUsLogAll = chpl_topo_getNumCPUsLogical(false);

    extern proc chpl_task_getMaxPar(): uint(32);
    dst.maxTaskPar = chpl_task_getMaxPar();

    extern proc chpl_get_num_colocales_on_node(): c_int;
    dst.numColocales = chpl_get_num_colocales_on_node();
  }

  proc helpSetupLocaleNUMA(dst:borrowed LocaleModel, out local_name:string, numSublocales, type NumaDomain) {
    helpSetupLocaleFlat(dst, local_name);

    extern proc chpl_task_getMaxPar(): uint(32);

    if numSublocales >= 1 {
      // These nPUs* values are estimates only; better values await
      // full hwloc support. In particular it assumes a homogeneous node
      const nPUsPhysAccPerSubloc = dst.nPUsPhysAcc/numSublocales;
      const nPUsPhysAllPerSubloc = dst.nPUsPhysAll/numSublocales;
      const nPUsLogAccPerSubloc = dst.nPUsLogAcc/numSublocales;
      const nPUsLogAllPerSubloc = dst.nPUsLogAll/numSublocales;
      const maxTaskParPerSubloc = chpl_task_getMaxPar()/numSublocales;
      const origSubloc = chpl_task_getRequestedSubloc(); // this should be any
      for i in dst.childSpace {
        // allocate the structure on the proper sublocale
        chpl_task_setSubloc(i:chpl_sublocID_t);
        dst.childLocales[i] = new unmanaged NumaDomain(i:chpl_sublocID_t,
                                                       new locale(dst));
        dst.childLocales[i].nPUsPhysAcc = nPUsPhysAccPerSubloc;
        dst.childLocales[i].nPUsPhysAll = nPUsPhysAllPerSubloc;
        dst.childLocales[i].nPUsLogAcc = nPUsLogAccPerSubloc;
        dst.childLocales[i].nPUsLogAll = nPUsLogAllPerSubloc;
        dst.childLocales[i].maxTaskPar = maxTaskParPerSubloc;
      }
      chpl_task_setSubloc(origSubloc);
    }
  }

  proc helpSetupLocaleGPU(dst: borrowed LocaleModel, out local_name:string,
      numSublocales: int, type GPULocale){

    local_name = getNodeName();

    extern proc chpl_topo_getNumCPUsPhysical(accessible_only: bool): c_int;
    dst.nPUsPhysAcc = chpl_topo_getNumCPUsPhysical(true);
    dst.nPUsPhysAll = chpl_topo_getNumCPUsPhysical(false);

    extern proc chpl_topo_getNumCPUsLogical(accessible_only: bool): c_int;
    dst.nPUsLogAcc = chpl_topo_getNumCPUsLogical(true);
    dst.nPUsLogAll = chpl_topo_getNumCPUsLogical(false);

    // Cyclic (and likely other) distributions uses this variable to determine
    // how many data-parallel tasks to have per locale. If this gets set to 0
    // then we end up not processing things in the first locale.
    extern proc chpl_task_getMaxPar(): uint(32);
    dst.maxTaskPar = chpl_task_getMaxPar();

    extern proc chpl_get_num_colocales_on_node(): c_int;
    dst.numColocales = chpl_get_num_colocales_on_node();

    var childSpace = {0..#numSublocales};

    const origSubloc = chpl_task_getRequestedSubloc();

    for i in childSpace {
      chpl_task_setSubloc(i:chpl_sublocID_t);
      dst.childLocales[i] = new unmanaged GPULocale(i:chpl_sublocID_t, dst);
      dst.childLocales[i].maxTaskPar = 1;

      dst.gpuSublocales[i] = new locale(dst.childLocales[i]);
    }
    chpl_task_setSubloc(origSubloc);
  }
}
