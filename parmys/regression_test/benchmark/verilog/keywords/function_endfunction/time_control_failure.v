module simple_op(in1,in2,in3,out);

    input  in1;
	 input  in2;
	 input  in3;
    output out;

    $display("Expect::FUNCTION test should fail, time control is not allowed in a function");

    assign out = my_and(in1,in2,in3);

    function my_and;
	 input a;
	 input b;
	 input c;
    begin
	  always @ (posedge c) begin
		my_and = (a & b) || c;
		end
    end
    endfunction
endmodule 