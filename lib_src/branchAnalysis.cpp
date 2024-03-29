#include "branchAnalysis.h"

size_t writeFunction(void* ptr, size_t size, size_t nmemb, std::string* data) {
    data->append((char*)ptr, size * nmemb);
    return size * nmemb;
}

json branchAnalysis::GetFromApi(std::string branchName) {
    auto curl = curl_easy_init();
    if (curl) {
        std::string url = "http://rdb.altlinux.org/api/export/branch_binary_packages/" + branchName;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 50L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);

        std::string response_string;
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFunction);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
            curl_easy_cleanup(curl);
            return {};
        }

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (http_code == 302) {
            char* new_location;
            curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &new_location);
            if (new_location) {
                curl_easy_setopt(curl, CURLOPT_URL, new_location);
                response_string.clear();
                res = curl_easy_perform(curl);
                if (res != CURLE_OK) {
                    std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
                    curl_easy_cleanup(curl);
                    return {};
                }
            } else {
                std::cerr << "Redirect location not provided" << std::endl;
                curl_easy_cleanup(curl);
                return {};
            }
        }

        curl_easy_cleanup(curl);

        if (!response_string.empty()) {
            return json::parse(response_string);
        } else {
            std::cerr << "Empty response" << std::endl;
        }
    } else {
        std::cerr << "Failed to initialize curl" << std::endl;
    }
    return {};
}

std::vector<std::string> branchAnalysis::split(const std::string& s) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);

    while (std::getline(tokenStream, token, '.')) {
        tokens.push_back(token);
    }

    return tokens;
}

bool branchAnalysis::isNumeric(const std::string& str) {
    return std::all_of(str.begin(), str.end(), [](char c) { return std::isdigit(c); });
}

int branchAnalysis::compareVersions(const std::string& v1, const std::string& v2) {
    std::vector<std::string> version1 = split(v1);
    std::vector<std::string> version2 = split(v2);

    size_t max_size = std::max(version1.size(), version2.size());

    for (size_t i = 0; i < max_size; ++i) {
        if (i >= version1.size()) return -1;
        if (i >= version2.size()) return 1;

        if (isNumeric(version1[i]) && isNumeric(version2[i])) {
            int num1 = std::stoi(version1[i]);
            int num2 = std::stoi(version2[i]);

            if (num1 > num2) return 1;
            if (num1 < num2) return -1;

        } else {
            const std::string& str1 = version1[i];
            const std::string& str2 = version2[i];
            size_t len1 = str1.length();
            size_t len2 = str2.length();
            size_t minLen = std::min(len1, len2);

            for (size_t j = 0; j < minLen; ++j) {
                if (str1[j] > str2[j]) return 1;
                if (str1[j] < str2[j]) return -1;
            }

            if (len1 > len2) return 1;
            if (len1 < len2) return -1;
        }
    }

    return 0;
}

void branchAnalysis::FindDiff(const std::shared_ptr<json>& data1, const std::shared_ptr<json>& data2) {
    json& packages1 = (*data1)["packages"];
    json& packages2 = (*data2)["packages"];

    std::unordered_map<std::string, json> packages1_map;
    for (const auto& item : packages1) {
        packages1_map[item["name"]] = item;
    }

    std::unordered_map<std::string, json> packages2_map;
    for (const auto& item : packages2) {
        packages2_map[item["name"]] = item;
    }

    json notInFirst;
    json notInSecond;
    json versionNew;

    for (const auto& item1 : packages1) {
        const auto& name = item1["name"];
        if (packages2_map.find(name) == packages2_map.end()) {
            notInSecond.push_back(item1);
        } else {
            const auto& item2 = packages2_map[name];
            if (compareVersions(item1["version"], item2["version"]) == 1) {
                versionNew.push_back(item1);
                notInSecond.push_back(item1);
            } else if (compareVersions(item1["version"], item2["version"]) == -1) {
                notInSecond.push_back(item1);
            }
        }
    }

    for (const auto& item2 : packages2) {
        const auto& name = item2["name"];
        if (packages1_map.find(name) == packages1_map.end()) {
            notInFirst.push_back(item2);
        } else {
            const auto& item1 = packages1_map[name];
            if (compareVersions(item1["version"], item2["version"])) {
                notInFirst.push_back(item2);
            }
        }
    }

    std::ofstream output1("notInSecond.json");
    output1 << std::setw(4) << notInSecond << std::endl;
    output1.close();
    std::cout << "Packages that contain only in first branch saved to build/notInSecond.json." << std::endl;

    std::ofstream output2("notInFirst.json");
    output2 << std::setw(4) << notInFirst << std::endl;
    output2.close();
    std::cout << "Packages that contain only in second branch saved to build/notInFirst.json." << std::endl;

    std::ofstream output3("versionNew.json");
    output3 << std::setw(4) << versionNew << std::endl;
    output3.close();
    std::cout << "Packages that are newer in first branch saved to build/versionNew.json." << std::endl;
}

void branchAnalysis::executeAnalysis(std::string branch1, std::string branch2) {
    json data1, data2;
    std::thread t1([this, &data1, branch1]() { data1 = GetFromApi(branch1); });
    std::thread t2([this, &data2, branch2]() { data2 = GetFromApi(branch2); });
    t1.join();
    t2.join();
    if (!data1.empty() && !data2.empty()) {
        FindDiff(std::make_shared<json>(data1), std::make_shared<json>(data2));
    }

    return;
}
