package TestAPR::table;

use strict;
use warnings FATAL => 'all';

use Apache::Test;

use APR::Table ();

use Apache::Const -compile => 'OK';

my $filter_count;
my $TABLE_SIZE = 20;

sub handler {
    my $r = shift;

    plan $r, tests => 17;

    my $table = APR::Table::make($r->pool, $TABLE_SIZE);

    ok (UNIVERSAL::isa($table, 'APR::Table'));

    ok $table->set('foo','bar') || 1;

    # scalar context
    ok $table->get('foo') eq 'bar';

    # add + list context
    $table->add(foo => 'tar');
    $table->add(foo => 'kar');
    my @array = $table->get('foo');
    ok @array == 3        &&
       $array[0] eq 'bar' &&
       $array[1] eq 'tar' &&
       $array[2] eq 'kar';

    ok $table->unset('foo') || 1;

    ok not defined $table->get('foo');

    for (1..$TABLE_SIZE) {
        $table->set(chr($_+97), $_);
    }

    #Simple filtering
    $filter_count = 0;
    $table->do("my_filter");
    ok $filter_count == $TABLE_SIZE;

    #Filtering aborting in the middle
    $filter_count = 0;
    $table->do("my_filter_stop");
    ok $filter_count == int($TABLE_SIZE)/2;

    #Filtering with anon sub
    $filter_count=0;
    $table->do(sub {
        my ($key,$value) = @_;
        $filter_count++;
        unless ($key eq chr($value+97)) {
            die "arguments I recieved are bogus($key,$value)";
        }
        return 1;
    });

    ok $filter_count == $TABLE_SIZE;

    $filter_count = 0;
    $table->do("my_filter", "c", "b", "e");
    ok $filter_count == 3;

    #Tied interface
    {
        my $table = APR::Table::make($r->pool, $TABLE_SIZE);

        ok (UNIVERSAL::isa($table, 'HASH'));

        ok (UNIVERSAL::isa($table, 'HASH')) && tied(%$table);

        ok $table->{'foo'} = 'bar';

        # scalar context
        ok $table->{'foo'} eq 'bar';

        ok delete $table->{'foo'} || 1;

        ok not exists $table->{'foo'};

        for (1..$TABLE_SIZE) {
            $table->{chr($_+97)} = $_;
        }

        $filter_count = 0;
        foreach my $key (sort keys %$table) {
            my_filter($key, $table->{$key});
        }
        ok $filter_count == $TABLE_SIZE;
    }

    Apache::OK;
}

sub my_filter {
    my ($key,$value) = @_;
    $filter_count++;
    unless ($key eq chr($value+97)) {
        die "arguments I received are bogus($key,$value)";
    }
    return 1;
}

sub my_filter_stop {
    my ($key,$value) = @_;
    $filter_count++;
    unless ($key eq chr($value+97)) {
        die "arguments I received are bogus($key,$value)";
    }
    return 0 if ($filter_count == int($TABLE_SIZE)/2);
    return 1;
}

1;
