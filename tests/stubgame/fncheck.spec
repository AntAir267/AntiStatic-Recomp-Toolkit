# The stub game's per-function check (tests/stubgame/run.sh; docs/REWRITE.md "Checking a function no
# route reaches"): stub_mix's rewrite (port/rewrite.c) against its translation, before the program's
# entry point (frame 0). @MIX@ is stub_mix's address, filled in by run.sh from the export table.
frame 0
timeout 20

# the input the stub game itself gives, its second half by a name the port gives (FCNAME mix_b)
call mix_route fn=@MIX@ args=0x12345678,mix_b expect=return
# inputs it never gives: STUB_MIX_MUTANT=1 breaks the rewrite on a = 0xdeadbeef only
call mix_other fn=@MIX@ args={0|0xdeadbeef|0xffffffff},{0|0x80000001}
# a C case (FNCASE mix_sweep): five inputs, the second read from a block it made with fc_new
case mix_sweep
