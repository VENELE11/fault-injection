DIRS = access-control-fi cpu-fi file-fi memory-fi memory-manage-fi state-query-fi vm-migration-fi hadoop-fi cloudstack-fi

all: cluster_controller subdirs
	@echo "=== All modules built successfully ==="

subdirs:
	for i in $(DIRS); do \
		(cd $$i && echo "=== Building $$i ===" && $(MAKE)) || exit 1; \
	done

cluster_controller: cluster_controller.c
	gcc -Wall -O2 -o cluster_controller cluster_controller.c
	@echo "=== Cluster Controller Built ==="

clean:
	for i in $(DIRS); do \
		(cd $$i && echo "=== Cleaning $$i ===" && $(MAKE) clean) || exit 1; \
	done
	rm -f cluster_controller

.PHONY: all clean subdirs
