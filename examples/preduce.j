# figure 5.2 from principles of parallel programming, ported to julia.
# sum a vector using a tree on top of local reductions.
function sum(v::DArray)
    P = procs(v)
    nodeval = { RemoteRef(p) | p=P }
    answer = RemoteRef()
    np = numel(P)

    for i=0:np-1
        @spawnat P[i+1] begin
            stride=1
            tally = sum(localize(v))
            while stride < np
                if i%(2*stride) == 0
                    tally = tally + take(nodeval[i+stride])
                    stride = 2*stride
                else
                    put(nodeval[i], tally)
                    break
                end
            end
            if i==0
                put(answer, tally)
            end
        end
    end
    fetch(answer)
end

function reduce(f, v::DArray)
    mapreduce(f, fetch,
              { @spawnat p reduce(f,localize(v)) | p = procs(v) })
end

# possibly-useful abstraction:

type RefGroup
    refs::Array{RemoteRef,1}

    RefGroup(P) = new([ RemoteRef(p) | p=P ])
end

ref(r::RefGroup, i) = fetch(r.refs[i])
assign(r::RefGroup, v, i) = put(r.refs[i], v)
