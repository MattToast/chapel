.. _best-practices-valgrind:

Valgrind
========

``valgrind`` is a tool that can help to check for memory errors.  See
also :ref:`best-practices-sanitizers` for an alternative strategy.

How-to 
------

To run ``valgrind`` on an executable generated by the ``chpl`` compiler:

.. code-block:: bash

     export CHPL_TARGET_MEM=cstdlib
     export CHPL_TASKS=fifo
     export CHPL_RT_NUM_THREADS_PER_LOCALE=450
     export CHPL_RE2_VALGRIND_SUPPORT=true

     cd $CHPL_HOME
     make
     chpl <program.chpl>
     valgrind ./<program>

This may require clobbering and rebuilding the ``re2`` library:

.. code-block:: bash

     cd $CHPL_HOME/third-party/re2
     make clobber && make

Explanation
-----------

The above options are needed because not all third-party libraries support
``valgrind``. In particular:

- ``jemalloc`` does not support ``valgrind``, which prevents ``valgrind`` from
  accurately tracking allocations/frees
- ``qthreads`` performs task-switching in user-space, so ``valgrind`` is not
  able to keep track of the stack frame correctly
- ``fifo`` needs to limit the number of threads to stay below valgrind's
  ``--max-threads`` default of 500.
- ``re2`` intentionally leaves some memory uninitialized for performance
  reasons, unless ``CHPL_RE2_VALGRIND_SUPPORT=true`` is set at build time
- GASNet support for ``valgrind`` is experimental at this time -- see 
  https://github.com/chapel-lang/chapel/issues/8544 for the current status

LLVM Compatibility
------------------

Some versions of LLVM generate debugging information that some versions
of valgrind can't understand.  This can manifest as an error like the following:

.. code-block:: text

    ### unhandled dwarf2 abbrev form code 0x25
    ### unhandled dwarf2 abbrev form code 0x25
    ### unhandled dwarf2 abbrev form code 0x25
    ### unhandled dwarf2 abbrev form code 0x1b
    ### unhandled dwarf2 abbrev form code 0x25
    ### unhandled dwarf2 abbrev form code 0x25
    ### unhandled dwarf2 abbrev form code 0x25
    ### unhandled dwarf2 abbrev form code 0x1b
    ==3996084== Valgrind: debuginfo reader: ensure_valid failed:
    ==3996084== Valgrind:   during call to ML_(img_get)
    ==3996084== Valgrind:   request for range [8566654, +4) exceeds
    ==3996084== Valgrind:   valid image size of 1676416 for image:
    ==3996084== Valgrind:   "/path/to/program"
    ==3996084==
    ==3996084== Valgrind: debuginfo reader: Possibly corrupted debuginfo file.
    ==3996084== Valgrind: I can't recover.  Giving up.  Sorry.
    ==3996084==

Use a non-LLVM backend, or configure LLVM to use an older debugging
format (adjusting the ``clang-`` versions below to match yours):

.. code-block:: bash

    export CHPL_TARGET_CC='clang-15 -gdwarf-4'
    export CHPL_TARGET_CXX='clang++-15 -gdwarf-4'
    export CHPL_TARGET_LD='clang++-15 -gdwarf-4'

Rebuild the runtime with those settings, and then use them when
building the program under test.

This should no longer be required if you have ``valgrind`` version 3.20 or later.
