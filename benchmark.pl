#!/usr/bin/perl

use strict;
use warnings;
use Time::HiRes qw(gettimeofday tv_interval);

# Generate a Pascal program that performs a large number of additions
my $pascal_file = "benchmark_add.pas";
open(my $fh, '>', $pascal_file) or die "Could not open file '$pascal_file' $!";

print $fh <<'END_PASCAL';
program BenchmarkAdd;
var
  i: LongInt;
  sum: LongInt;
begin
  sum := 0;
  for i := 1 to 100000000 do
  begin
    sum := sum + 1;
  end;
  WriteLn('Sum: ', sum);
end.
END_PASCAL

close $fh;

# Measure execution time
print "Running benchmark...\n";
my $start_time = [gettimeofday];
# Run pascal directly. It compiles and executes.
# We assume the compilation time is constant and small relative to 100M iterations.
if (system("./pascal", $pascal_file) != 0) {
    die "Execution failed";
}
my $end_time = [gettimeofday];

my $elapsed = tv_interval($start_time, $end_time);
print "Total execution time: $elapsed seconds\n";

# Cleanup
unlink $pascal_file;
# unlink "benchmark_add.obc"; # Pascal might produce this or cache it
