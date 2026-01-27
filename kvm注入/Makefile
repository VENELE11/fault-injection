DIRS = access-control-fi cpu-fi file-fi memory-fi memory-manage-fi state-query-fi vm-migration-fi

all:
    for i in $(DIRS); do \
        (cd $$i && echo "=== Building $$i ===" && $(MAKE)) || exit 1; \
    done

clean:
    for i in $(DIRS); do \
        (cd $$i && echo "=== Cleaning $$i ===" && $(MAKE) clean) || exit 1; \
    done