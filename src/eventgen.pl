#!/usr/local/bin/perl -w
# impl.pl.eventgen: GENERATOR FOR impl.h.eventgen
#
# Copyright (C) 1998 Harlequin Group plc.  All rights reserved.
# $HopeName: MMsrc!eventgen.pl(trunk.8) $
#
# .how: Invoke this script in the src directory.  It works by scanning
# eventdef.h and then creating a file eventgen.h that includes the
# necessary types and macros.  If the format letter doesn't
# exist as a WriteF format type, then the subsequent compile
# will fail.
#
# You will need to have eventgen.h claimed, and you should
# remember to check it in afterwards.

$HopeName = '$HopeName: MMsrc!eventgen.pl(trunk.8) $';

%Formats = ();

%Types = (
  "D", "double",
  "S", "char *",
  "U", "unsigned",
  "W", "Word",
  "A", "Addr",
  "P", "void *",
	  );


open(C, "<eventdef.h") || die "Can't open $_";
while(<C>) {
  if(/RELATION\([^,]*,[^,]*,[^,]*,[^,]*, ([A-Z]+)\)/) { 
    $Formats{$1} = 1 if(!defined($Formats{$1}));
  }
}
close(C);


open(H, ">eventgen.h") || die "Can't open eventgen.h for output";

print H "/* impl.h.eventgen -- Automatic event header
 *
 * \$HopeName\$
 *
 * DO NOT EDIT THIS FILE!
 * This file was generated by", substr($HopeName, 10), "
 */\n\n";

print H "#ifndef eventgen_h\n#define eventgen_h\n\n";
print H "#include \"mpmtypes.h\"\n\n";
# Should include mpm.h for IndexAlignUp &c., but that would recurse.

print H "#ifdef EVENT\n\n";


foreach $format ("", sort(keys(%Formats))) {
  $fmtcode = ($format eq "") ? "0" : $format;
  print H "typedef struct {\n";
  print H "  Word code;\n  Word length;\n  Word clock;\n";
  for($i = 0; $i < length($format); $i++) {
    $c = substr($format, $i, 1);
    if($c eq "S") {
      die "String must be at end of format" if($i+1 != length($format));
      print H "  char s$i[EventStringLengthMAX];\n";
    } elsif(!defined($Types{$c})) {
      die "Can't find type for format code >$c<.";
    } else {
      print H "  ", $Types{$c}, " \l$c$i;\n";
    }
  }
  print H "} Event${fmtcode}Struct;\n\n";
  print H "#define EVENT_${fmtcode}_FIELD_PTR(event, i) \\\n  (";
  for($i = 0; $i < length($format); $i++) {
    $c = substr($format, $i, 1);
    print H "((i) == $i) ? (void *)&((event)->\L$fmtcode.$c\E$i) \\\n   : ";
  }
  print H "NULL)\n\n";
}


print H "\ntypedef union {\n  Event0Struct any;\n";

foreach $format (sort(keys(%Formats))) {
  print H "  Event${format}Struct \L$format;\n";
}
print H "} EventUnion;\n\n\n";


print H "#define EVENT_0(type) \\
  EVENT_BEGIN(type, 0, sizeof(Event0Struct)) \\
  EVENT_END(type, sizeof(Event0Struct))\n\n";

foreach $format (sort(keys(%Formats))) {
  print H "#define EVENT_$format(type";

  for($i = 0; $i < length($format); $i++) {
    $c = substr($format, $i, 1);
    if($c eq "S") {
      print H ", _s";
    } else {
      print H ", _\l$c$i";
    }
  }

  print H ") \\\n";

  print H "  BEGIN \\\n";

  if(($i = index($format, "S")) != -1) {
    print H "    const char *_s2;\\\n";
    print H "    size_t _string_length, _length;\\\n";
    print H "    _s2 = (_s); \\\n"; # May be literal
    print H "    _string_length = StringLength((_s2)); \\\n";
    print H "    AVER(_string_length < EventStringLengthMAX);\\\n";
    print H "    _length = \\\n";
    print H "      IndexAlignUp(offsetof(Event${format}Struct, s$i) "
                             . "+ _string_length + 1, \\\n";
    print H "                   sizeof(Word)); \\\n";
  } else {
    print H "    size_t _length = sizeof(Event${format}Struct); \\\n";
  }

  print H "    EVENT_BEGIN(type, $format, _length); \\\n";

  for($i = 0; $i < length($format); $i++) {
    $c = substr($format, $i, 1);
    if($c eq "S") {
      print H "    mps_lib_memcpy(EventMould.\L$format.s$i, (_s2), "
                               . "_string_length + 1); \\\n";
    } else {
      print H "    EventMould.\L$format.$c$i = (_$c$i); \\\n";
    }
  }

    print H "    EVENT_END(type, _length); \\\n";
    print H "  END\n\n";
}

$C = 0;
foreach $format ("0", sort(keys(%Formats))) {
  print H "#define EventFormat$format $C\n";
  $C++;
}

print H "\n#else /* EVENT not */\n\n";

print H "#define EVENT_0(type) NOOP\n";

foreach $format (sort(keys(%Formats))) {
  print H "#define EVENT_$format(type";
  for($i = 0; $i < length($format); $i++) {
    print H ", p$i";
  }
  print H ") NOOP\n";
}

print H "\n#endif /* EVENT */\n";

print H "\n#endif /* eventgen_h */\n";

close(H);
