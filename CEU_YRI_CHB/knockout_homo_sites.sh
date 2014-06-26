#!/bin/bash

ORG_DATA_DIR="$HOME/Desktop/CEUYRICHB_data/"
REDUCED_DIR="$HOME/Desktop/CEUYRICHB_reduced_data/"
mkdir ${REDUCED_DIR}

# Afrian data YRI NA18501, NA18502
indv1="NA18501"
indv2="NA18502"

# European ancenstry (Utah) CEU NA06985, NA06994
indv3="NA06994"
indv4="NA12890"

# Chinese population CHB NA18525, NA18526
indv5="NA18525"
indv6="NA18526"

for chr in $(seq 1 1 1)
    do 
    VCFfile="${indv1}_${indv2}_${indv3}_${indv4}_${indv5}_${indv6}_CHROM"${chr}.vcf
    awk '$10!="0|0"||$11!="0|0"||$12!="0|0"||$13!="0|0"||$14!="0|0"||$15!="0|0"' ${ORG_DATA_DIR}${VCFfile} | awk '$10!="1|1"||$11!="1|1"||$12!="1|1"||$13!="1|1||$14!="1|1||$15!="1|1"' > ${REDUCED_DIR}${VCFfile}    
    done


