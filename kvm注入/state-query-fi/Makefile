DIRS = kvm-state-fi kvm-version-fi

all:
    for i in $(DIRS); do \
        (cd $$i && echo "make $$i" && $(MAKE)) || exit 1; \
    done

clean:
    for i in $(DIRS); do \
        (cd $$i && echo "cleaning $$i" && $(MAKE) clean) || exit 1; \
    done