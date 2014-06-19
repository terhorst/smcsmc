#!/bin/bash
#$ -cwd
#$ -V
#$ -P bsg.prjb -q short.qb
#$ -e open2sample10000npErrFiles
#$ -o open2sample10000npOutFiles
#$ -N open2sample10000np
#$ -t 1-15
#$ -j y

rep=$(expr $SGE_TASK_ID )
data_dir="/well/bsg/joezhu/experiment4_data/"
out_dir="/well/bsg/joezhu/experiment4/"

nsam=2
Np=10000
EM=20
pattern="1*4+25*2+1*4+1*6"
tmax=3

cmd="-EM ${EM} -Np ${Np} -t 10000 -r 6000 1000000 -p ${pattern} -tmax ${tmax} -l 0 -seed ${rep}"
case="open"

prefix=${case}Samples${nsam}msdata${rep}
outprefix=${out_dir}${prefix}Np${Np}
pf-ARG ${cmd} -o ${outprefix}  -vcf ${data_dir}${prefix}.vcf -log 
