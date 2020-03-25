##########################################################################
# If not stated otherwise in this file or this component's LICENSE
# file the following copyright and licenses apply:
#
# Copyright 2019 RDK Management
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
##########################################################################
user_config_file="opt/cvr_config.txt"
default_config_file="etc/cvr_config.txt"

if [ -f $default_config_file ]
then
	if grep -q "FILE_PATH" $default_config_file ;
	then
		while read var value
		do
        		if [ "$var" == "FILE_PATH" ]
        		then
        	        	export cvr_save_path="$value"
        		fi
		done < $default_config_file
	else
		echo "ERROR: $default_config_file has no parameter FILE_PATH"
		exit 1
	fi

else
	echo "ERROR: $default_config_file not found"
	exit 1
fi

if [ -f $user_config_file ]
then
	echo "Found user defined config file $user_config_file"
        if grep -q "FILE_PATH"  $user_config_file ;
        then
                while read var value
                do
                        if [ "$var" == "FILE_PATH" ]
                        then
                                export cvr_save_path="$value"
				echo "Using user defined save location $cvr_save_path"
                        fi
                done < $user_config_file
        else
                echo "$user_config_file has no parameter FILE_PATH"
		echo "Using default save location $cvr_save_path"
        fi
else
 	echo "Using default save location $cvr_save_path"
fi

if [ -d $cvr_save_path ]
then
	echo "Save directory $cvr_save_path already exists"
else
	mkdir -p $cvr_save_path
fi

while :
do
	find $cvr_save_path -name "*.ts" -mmin +1 | xargs rm -rf
done








