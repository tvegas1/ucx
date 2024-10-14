#!/usr/bin/awk -f

BEGIN {
    OFS="\t"
}

function emit(type, name) {
    print name, FILENAME, "/^" $0 "$/;\"", "f", "typeref:typename:"type, "file:"
}

/UCS_PROFILE_FUNC_VOID[\t ]*\(/ {
    match($0, /UCS_PROFILE_FUNC_VOID\([\t ]*([^, \t]+)/, name)
    if (name[1] == "") {
        print "Failed to get function name for:" | "cat >&2"
        print $0 | "cat >&2"
        next
    }

    emit("void", name[1]);
}

/UCS_PROFILE_FUNC[\t ]*\(/ {
    match($0, /UCS_PROFILE_FUNC\([\t ]*([^, \t]+)/, type)
    match($0, /UCS_PROFILE_FUNC\([^,]+,[\t ]*([^, \t]+)/, name)
    if (type[1] == "" || name[1] == "") {
        print "Failed to get function name for:" | "cat >&2"
        print $0 | "cat >&2"
        next
    }

    emit(type[1], name[1]);
}
